// Named provider profiles: one connection per account, switchable without
// editing config or restarting. The store, the masking and the precedence are
// pure lib pieces; the TUI/menu surface is covered by the pty suite.

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "agent/config.h"
#include "agent/global_config.h"
#include "agent/profiles.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

namespace agent {

// ASSERT_EQ prints both operands; the enum has no production stream operator.
inline std::ostream& operator<<(std::ostream& os, AgentMode mode) {
    return os << static_cast<int>(mode);
}

} // namespace agent

namespace {

namespace fs = std::filesystem;

bool write_text(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

mode_t mode_of(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return st.st_mode & 0777;
}

agent::Profile sample(const std::string& name) {
    agent::Profile p;
    p.name = name;
    p.provider_name = "work-gateway";
    p.flavor = "anthropic";
    p.api_base = "https://work.example/v1";
    p.api_key = "sk-work-1234567890abcd";
    p.model = "claude-work-1";
    p.temperature = 0.4;
    p.max_tokens = 4096;
    p.thinking = "off";
    return p;
}

} // namespace

TEST(profile_name_validation_rejects_a_path) {
    std::string why;
    ASSERT_TRUE(agent::valid_profile_name("work", &why));
    ASSERT_TRUE(agent::valid_profile_name("consulting.acme_2", &why));
    ASSERT_FALSE(agent::valid_profile_name("", &why));
    ASSERT_FALSE(agent::valid_profile_name(".", &why));
    ASSERT_FALSE(agent::valid_profile_name("..", &why));
    ASSERT_FALSE(agent::valid_profile_name("../escape", &why));
    ASSERT_FALSE(agent::valid_profile_name("a/b", &why));
    ASSERT_FALSE(agent::valid_profile_name("a b", &why));
    ASSERT_FALSE(agent::valid_profile_name(std::string(65, 'a'), &why));
    // A caller that only needs the verdict passes no reason.
    ASSERT_TRUE(agent::valid_profile_name("work"));
    ASSERT_FALSE(agent::valid_profile_name("a b"));
}

TEST(profile_key_is_always_masked) {
    ASSERT_EQ(agent::mask_key(""), std::string("(unset)"));
    ASSERT_EQ(agent::mask_key("short"), std::string("****"));
    ASSERT_EQ(agent::mask_key("12345678"), std::string("****"));
    ASSERT_EQ(agent::mask_key("sk-work-1234567890abcd"), std::string("sk-...abcd"));
    ASSERT(agent::mask_key("sk-work-1234567890abcd").find("1234567890") == std::string::npos);
}

TEST(profile_roundtrip_preserves_the_connection) {
    agent::test::XdgGuard guard("profiles_roundtrip");
    const agent::Profile saved = sample("work");
    std::string error;
    ASSERT_TRUE(agent::save_profile(saved, &error));
    ASSERT(error.empty());

    const auto loaded = agent::load_profile("work");
    ASSERT(loaded.has_value());
    ASSERT_EQ(loaded->name, saved.name);
    ASSERT_EQ(loaded->provider_name, saved.provider_name);
    ASSERT_EQ(loaded->flavor, saved.flavor);
    ASSERT_EQ(loaded->api_base, saved.api_base);
    ASSERT_EQ(loaded->api_key, saved.api_key);
    ASSERT_EQ(loaded->model, saved.model);
    ASSERT_EQ(loaded->temperature, saved.temperature);
    ASSERT_EQ(loaded->max_tokens, saved.max_tokens);
    ASSERT_EQ(loaded->thinking, saved.thinking);
}

TEST(profile_file_is_owner_only) {
    agent::test::XdgGuard guard("profiles_private");
    ASSERT_TRUE(agent::save_profile(sample("private")));
    ASSERT_EQ(mode_of(agent::profile_path("private")), static_cast<mode_t>(0600));
    ASSERT_EQ(mode_of(agent::profiles_dir()), static_cast<mode_t>(0700));
}

TEST(profile_list_is_sorted_and_ignores_junk) {
    agent::test::XdgGuard guard("profiles_list");
    ASSERT_TRUE(agent::save_profile(sample("zeta")));
    ASSERT_TRUE(agent::save_profile(sample("alpha")));
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/notes.txt", "not a profile"));
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/broken.json", "{not json"));
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/array.json", "[]"));
    // A JSON file whose stem is not a safe profile name is a stray file, not a
    // profile: the name is validated before it becomes a path component.
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/bad name.json", "{}"));
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/../escape.json", "{}"));

    const auto list = agent::load_profiles();
    ASSERT_EQ(list.size(), static_cast<size_t>(2));
    ASSERT_EQ(list[0].name, std::string("alpha"));
    ASSERT_EQ(list[1].name, std::string("zeta"));
}

TEST(profile_save_rejects_a_path_and_missing_profile_removal_is_reported) {
    agent::test::XdgGuard guard("profiles_refuse");
    std::string error;
    agent::Profile bad = sample("../escape");
    ASSERT_FALSE(agent::save_profile(bad, &error));
    ASSERT_FALSE(error.empty());
    ASSERT_FALSE(agent::remove_profile("ghost", &error));
    ASSERT(error.find("ghost") != std::string::npos);
    ASSERT_FALSE(agent::remove_profile("../escape", &error));
    ASSERT(error.find("'.'") != std::string::npos || error.find("letters") != std::string::npos);
}

TEST(profile_apply_replaces_the_connection_and_clears_the_pin) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    cfg.ui_theme = "modern";
    cfg.show_reasoning = false;
    cfg.provider_name = "old";
    cfg.api_base = "https://old.example/v1";
    cfg.api_key = "sk-old";
    cfg.model = "old-model";
    cfg.context_size = 256000;
    cfg.context_explicit = true;

    agent::apply_profile(cfg, sample("work"));
    ASSERT_EQ(cfg.profile_name, std::string("work"));
    ASSERT_EQ(cfg.provider_name, std::string("work-gateway"));
    ASSERT_EQ(cfg.flavor, std::string("anthropic"));
    ASSERT_EQ(cfg.api_base, std::string("https://work.example/v1"));
    ASSERT_EQ(cfg.api_key, std::string("sk-work-1234567890abcd"));
    ASSERT_EQ(cfg.model, std::string("claude-work-1"));
    ASSERT(cfg.model_explicit);
    ASSERT_EQ(cfg.temperature, 0.4);
    ASSERT_EQ(cfg.max_tokens, static_cast<size_t>(4096));
    ASSERT_EQ(cfg.thinking, std::string("off"));
    // A profile owns no UI or mode state.
    ASSERT_EQ(cfg.mode, agent::AgentMode::Yolo);
    ASSERT_EQ(cfg.ui_theme, std::string("modern"));
    ASSERT_FALSE(cfg.show_reasoning);
    // A pin measured against the old model does not survive the switch.
    ASSERT_FALSE(cfg.context_explicit);
    ASSERT_EQ(cfg.context_size, 0);
}

TEST(profile_apply_uses_safe_defaults_for_an_empty_definition) {
    agent::Profile empty;
    empty.name.clear();
    empty.provider_name.clear();
    empty.flavor.clear();
    empty.api_base.clear();
    empty.model.clear();
    agent::Config cfg;
    cfg.provider_name = "old";
    cfg.flavor = "anthropic";
    cfg.api_base = "https://old.example/v1";
    agent::apply_profile(cfg, empty);
    ASSERT_EQ(cfg.profile_name, std::string("default"));
    ASSERT_EQ(cfg.provider_name, std::string("custom"));
    ASSERT_EQ(cfg.flavor, std::string("openai"));
    ASSERT(cfg.api_base.empty());
    ASSERT_FALSE(cfg.api_base_from_provider);
    ASSERT_FALSE(cfg.model_explicit);
}

TEST(profile_load_rejects_absent_invalid_and_malformed_files) {
    agent::test::XdgGuard guard("profiles_bad");
    ASSERT_TRUE(agent::load_profiles().empty()); // the directory does not exist yet
    ASSERT_FALSE(agent::load_profile("absent").has_value());
    ASSERT_FALSE(agent::load_profile("../escape").has_value());
    ASSERT_TRUE(write_text(agent::profile_path("broken"), "{not json"));
    ASSERT_FALSE(agent::load_profile("broken").has_value());
    // A directory that happens to end in .json is not a profile.
    std::error_code ec;
    fs::create_directories(agent::profiles_dir() + "/directory.json", ec);
    ASSERT_TRUE(agent::load_profiles().empty());
}

TEST(profile_malformed_fields_fall_back_to_defaults) {
    agent::test::XdgGuard guard("profiles_fields");
    ASSERT_TRUE(write_text(agent::profile_path("odd"),
                           R"({"provider":7,"model":null,"temperature":"hot",)"
                           R"("max_tokens":-5,"thinking":"sometimes"})"));
    const auto odd = agent::load_profile("odd");
    ASSERT(odd.has_value());
    ASSERT_EQ(odd->provider_name, std::string("custom"));
    ASSERT(odd->model.empty());
    ASSERT_EQ(odd->temperature, 0.2);
    ASSERT_EQ(odd->max_tokens, static_cast<size_t>(16384));
    ASSERT_EQ(odd->thinking, std::string("auto"));
}

TEST(project_profile_without_a_pin_is_a_no_op) {
    agent::test::XdgGuard guard("profiles_no_pin");
    const std::string root = "/tmp/amber_profiles_no_pin";
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_TRUE(agent::project_profile_name(root).empty());
    fs::create_directories(root + "/.amber", ec);
    ASSERT_TRUE(write_text(root + "/.amber/profile", "# only a comment\n\n"));
    ASSERT_TRUE(agent::project_profile_name(root).empty());
    agent::Config cfg;
    std::string note = "untouched";
    ASSERT_FALSE(agent::apply_project_profile(cfg, root, &note));
    ASSERT_EQ(note, std::string("untouched"));
}

TEST(profile_store_failures_are_reported_not_thrown) {
    agent::test::XdgGuard guard("profiles_store_errors");
    // A file where the directory should be: the save cannot create the store.
    ASSERT_TRUE(write_text(agent::profiles_dir(), "not a directory"));
    std::string error;
    ASSERT_FALSE(agent::save_profile(sample("work"), &error));
    ASSERT(error.find("cannot create") != std::string::npos);

    // A non-empty directory named like a profile cannot be removed by the
    // file-store primitive; the command gets an error rather than a crash.
    std::error_code ec;
    fs::remove(agent::profiles_dir(), ec);
    fs::create_directories(agent::profiles_dir() + "/stuck.json", ec);
    ASSERT_TRUE(write_text(agent::profiles_dir() + "/stuck.json/keep", "x"));
    error.clear();
    ASSERT_FALSE(agent::remove_profile("stuck", &error));
    ASSERT(error.find("could not remove") != std::string::npos);
}

TEST(profile_write_failure_is_reported) {
    agent::test::XdgGuard guard("profiles_write_fail");
    ASSERT_TRUE(agent::save_profile(sample("first"))); // creates the profiles directory
    // A non-empty directory where the profile file belongs makes the atomic
    // rename fail; the command reports it instead of throwing.
    std::error_code ec;
    fs::create_directories(agent::profile_path("stuck"), ec);
    ASSERT_TRUE(write_text(agent::profile_path("stuck") + "/keep", "x"));
    std::string error;
    ASSERT_FALSE(agent::save_profile(sample("stuck"), &error));
    ASSERT(error.find("cannot write") != std::string::npos);
}

TEST(profile_default_migrates_from_the_current_config_once) {
    agent::test::XdgGuard guard("profiles_migrate");
    agent::Config cfg;
    cfg.provider_name = "custom";
    cfg.api_base = "http://127.0.0.1:8081/v1";
    cfg.model = "local-model";

    ASSERT_TRUE(agent::ensure_default_profile(cfg));
    auto def = agent::load_profile("default");
    ASSERT(def.has_value());
    ASSERT_EQ(def->provider_name, std::string("custom"));
    ASSERT_EQ(def->api_base, std::string("http://127.0.0.1:8081/v1"));
    ASSERT_EQ(def->model, std::string("local-model"));

    // A deliberately edited default is not overwritten by a later start.
    agent::Profile edited = *def;
    edited.model = "edited-model";
    ASSERT_TRUE(agent::save_profile(edited));
    ASSERT_FALSE(agent::ensure_default_profile(cfg));
    auto again = agent::load_profile("default");
    ASSERT(again.has_value());
    ASSERT_EQ(again->model, std::string("edited-model"));
}

TEST(project_profile_file_selects_a_profile) {
    agent::test::XdgGuard guard("profiles_project");
    const std::string root = "/tmp/amber_profiles_project";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/.amber", ec);
    ASSERT_TRUE(write_text(root + "/.amber/profile", "\n  work  \n# a comment\n"));
    ASSERT_EQ(agent::project_profile_name(root), std::string("work"));

    ASSERT_TRUE(agent::save_profile(sample("work")));
    agent::Config cfg;
    cfg.model = "before";
    std::string note;
    ASSERT_TRUE(agent::apply_project_profile(cfg, root, &note));
    ASSERT_EQ(cfg.profile_name, std::string("work"));
    ASSERT_EQ(cfg.model, std::string("claude-work-1"));
    ASSERT(note.find("work") != std::string::npos);

    ASSERT_TRUE(write_text(root + "/.amber/profile", "../escape\n"));
    std::string bad_note;
    ASSERT_FALSE(agent::apply_project_profile(cfg, root, &bad_note));
    ASSERT(bad_note.find("invalid") != std::string::npos);

    ASSERT_TRUE(write_text(root + "/.amber/profile", "ghost\n"));
    std::string ghost_note;
    ASSERT_FALSE(agent::apply_project_profile(cfg, root, &ghost_note));
    ASSERT(ghost_note.find("ghost") != std::string::npos);

    // The note is optional: a host that only wants the side effect passes none.
    ASSERT_TRUE(write_text(root + "/.amber/profile", "work\n"));
    ASSERT_TRUE(agent::apply_project_profile(cfg, root, nullptr));
    ASSERT_TRUE(write_text(root + "/.amber/profile", "ghost\n"));
    ASSERT_FALSE(agent::apply_project_profile(cfg, root, nullptr));
    ASSERT_TRUE(write_text(root + "/.amber/profile", "../escape\n"));
    ASSERT_FALSE(agent::apply_project_profile(cfg, root, nullptr));
}

TEST(project_profile_name_falls_back_to_the_workspace_root) {
    agent::test::XdgGuard guard("profiles_root_fallback");
    const std::string root = "/tmp/amber_profiles_root_fallback";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/.amber", ec);
    ASSERT_TRUE(write_text(root + "/.amber/profile", "work\n"));
    agent::Workspace::set_root(root);
    ASSERT_EQ(agent::project_profile_name(""), std::string("work"));
    fs::remove_all(root, ec);
}

TEST(apply_global_selection_reads_the_active_profile) {
    agent::test::XdgGuard guard("profiles_global");
    ASSERT_TRUE(agent::save_profile(sample("work")));
    ASSERT_TRUE(write_text(agent::global_config_path(), "provider=custom\n"
                                                        "api_base=https://selection.example/v1\n"
                                                        "model=selection-model\n"
                                                        "profile=work\n"));

    agent::Config cfg;
    const agent::GlobalConfig global = agent::apply_global_selection(cfg);
    ASSERT(global.loaded);
    ASSERT_TRUE(global.profile_applied);
    ASSERT_EQ(cfg.profile_name, std::string("work"));
    ASSERT_EQ(cfg.api_base, std::string("https://work.example/v1"));
    ASSERT_EQ(cfg.model, std::string("claude-work-1"));
}

TEST(global_model_does_not_override_an_active_profile) {
    agent::test::XdgGuard guard("profiles_model_layering");
    ASSERT_TRUE(agent::save_profile(sample("work")));
    ASSERT_TRUE(write_text(agent::global_config_path(), "provider=custom\n"
                                                        "model=stale-global-model\n"
                                                        "profile=work\n"));
    agent::Config cfg;
    const agent::GlobalConfig global = agent::apply_global_selection(cfg);
    agent::apply_global_model(cfg, global);
    ASSERT_EQ(cfg.model, std::string("claude-work-1"));
}

TEST(profile_name_round_trips_through_the_global_config) {
    agent::test::XdgGuard guard("profiles_config_roundtrip");
    agent::Config cfg;
    cfg.profile_name = "work";
    cfg.provider_name = "gateway";
    cfg.model = "m";
    ASSERT_TRUE(cfg.save_global(agent::global_config_path()));

    agent::Config loaded;
    loaded.load(agent::global_config_path());
    ASSERT_EQ(loaded.profile_name, std::string("work"));

    agent::Config def;
    def.profile_name = "default";
    ASSERT_TRUE(def.save_global(agent::global_config_path()));
    std::ifstream in(agent::global_config_path());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT(text.find("profile=") == std::string::npos);
}

TEST(project_config_cannot_half_select_a_profile) {
    agent::test::XdgGuard guard("profiles_project_layer");
    const std::string path = guard.dir + "/amber.conf";
    ASSERT_TRUE(write_text(path, "profile=other\napi_base=https://project.example/v1\n"));
    agent::Config cfg;
    cfg.profile_name = "work";
    cfg.load_workspace_defaults(path);
    ASSERT_EQ(cfg.profile_name, std::string("work"));
}

TEST(missing_profile_warns_and_keeps_the_selection) {
    agent::test::XdgGuard guard("profiles_missing");
    ASSERT_TRUE(write_text(agent::global_config_path(), "provider=custom\n"
                                                        "api_base=https://selection.example/v1\n"
                                                        "model=selection-model\n"
                                                        "profile=ghost\n"));
    agent::Config cfg;
    const agent::GlobalConfig global = agent::apply_global_selection(cfg);
    ASSERT_FALSE(global.profile_applied);
    ASSERT_EQ(cfg.profile_name, std::string("default"));
    bool warned = false;
    for (const auto& warning : cfg.warnings)
        warned = warned || warning.find("ghost") != std::string::npos;
    ASSERT(warned);
}
