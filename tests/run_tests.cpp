
#include <atomic>
#include <csignal>
#include <future>
#include <thread>
#include "agent.h"
#include "agent/tools.h"
#include "agent/artifacts.h"
#include "agent/search_backend.h"
#include "agent/dialect.h"
#include "agent/dialect_openai.h"
#include "agent/stream_decoder.h"
#include "agent/compressor.h"
#include "agent/context_shape.h"
#include "agent/global_config.h"
#include "agent/dispatch.h"
#include "agent/experience.h"
#include "agent/environment.h"
#include "../lib/http_transport.h"
#include "agent/data_path.h"
#include "agent/bootstrap.h"
#include "agent/model_probe.h"
#include "agent/plugin_runtime.h"
#include "plugins/amber_self/amber_self_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "agent/tool_call_parser.h"
#include "agent/todo.h"
#include "agent/session_brief.h"
#include "agent/skill_file.h"
#include "agent/skill_install.h"
#include "agent/config_paths.h"
#include "agent/private_file.h"
#include "agent/mcp_tools.h"
#include "agent/subagent.h"
#include "agent/repair.h"
#include "agent/run_scope.h"
#include "agent/plugin.h"
#include "agent/turn.h"
#include "tests/fake_llm.h"
#include "tests/test_util.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static inline void run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    (void)rc;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(config_defaults) {
    agent::Config c;
    ASSERT_EQ(c.api_base, "http://localhost:8000/v1");
    ASSERT_EQ(c.model, "gpt-4o-mini");
    ASSERT_EQ(c.max_tool_iterations, 100);
    ASSERT_TRUE(c.stream);
    ASSERT_EQ(agent::make_dialect(c.flavor)->chat_url(c),
              "http://localhost:8000/v1/chat/completions");
}

TEST(config_global_dir_honours_absolute_xdg) {
    setenv("XDG_CONFIG_HOME", "/tmp/amber_cfg_abs", 1);
    ASSERT_EQ(agent::global_config_dir(), "/tmp/amber_cfg_abs/amber");
    unsetenv("XDG_CONFIG_HOME");
}

TEST(config_global_dir_ignores_a_hostile_xdg) {
    // The root comes from the environment, so a relative or traversal value
    // must not redirect where amber reads and writes: fall back to HOME.
    const char* home = std::getenv("HOME");
    ASSERT(home != nullptr);
    const std::string expect = std::string(home) + "/.config/amber";
    setenv("XDG_CONFIG_HOME", "relative/path", 1);
    ASSERT_EQ(agent::global_config_dir(), expect);
    setenv("XDG_CONFIG_HOME", "/tmp/../etc", 1);
    ASSERT_EQ(agent::global_config_dir(), expect);
    unsetenv("XDG_CONFIG_HOME");
}

// Diagnostics decide where the raw HTTP/SSE dump goes: nothing with them off,
// a derived path under the state dir with them on, and an explicitly configured
// path always wins (the advanced escape hatch).
TEST(config_debug_dump_path_follows_the_switch) {
    agent::Config c;
    ASSERT_EQ(c.debug_dump_path(), std::string());

    c.debug = true;
    const std::string dump = c.debug_dump_path();
    ASSERT(dump.find("/logs/debug-") != std::string::npos);
    // The placeholder is expanded (one dump per process), never left literal.
    ASSERT(dump.find('{') == std::string::npos);
    ASSERT(dump.size() > 4 && dump.substr(dump.size() - 4) == ".log");
    ASSERT(dump.find(agent::Workspace::state_dir()) == 0);

    c.debug_log = "/tmp/explicit-http.log";
    ASSERT_EQ(c.debug_dump_path(), std::string("/tmp/explicit-http.log"));
    c.debug = false;
    ASSERT_EQ(c.debug_dump_path(), std::string("/tmp/explicit-http.log"));
}

TEST(config_validate_accepts_defaults) {
    agent::Config c;
    ASSERT_TRUE(c.validate().empty());
}

TEST(config_validate_flags_problems) {
    agent::Config c;
    c.api_base = "localhost:8000/v1"; // missing scheme
    c.model = "";                     // empty
    c.max_tool_iterations = 0;        // too small
    c.temperature = 5.0;              // out of range
    c.max_tokens = 0;                 // zero
    c.thinking = "sometimes";         // invalid enum
    auto errs = c.validate();
    ASSERT(errs.size() >= 6);

    agent::Config trailing;
    trailing.api_base = "http://localhost:8000/v1/"; // trailing slash
    ASSERT_FALSE(trailing.validate().empty());
}

TEST(config_load_key_value) {
    std::string path = "/tmp/amber_cfg_test.txt";
    {
        std::ofstream f(path);
        f << "# comment\n";
        f << "model=\"my-model\"\n";
        f << "api_base=http://example:1234/v1\n";
        f << "max_tool_iterations=5\n";
        f << "temperature=0.9\n";
        f << "stream=false\n";
        f << "subagent_parallel=false\n";
        f << "subagent_max=2\n";
        f << "reasoning_effort=high\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_EQ(c.api_base, "http://example:1234/v1");
    ASSERT_EQ(c.max_tool_iterations, 5);
    ASSERT_EQ(c.temperature, 0.9);
    ASSERT_FALSE(c.stream);
    ASSERT_FALSE(c.subagent_parallel);
    ASSERT_EQ(c.subagent_max, 2);
    ASSERT_EQ(c.reasoning_effort, "high");
    std::remove(path.c_str());
}

// Every key the parser knows, in one file. Parsing is a dispatch over ~40 keys
// split into per-domain appliers, so this is the test that a key cannot be
// dropped or wired to the wrong field.
TEST(config_load_covers_every_key) {
    std::string path = "/tmp/amber_cfg_all_keys.conf";
    {
        std::ofstream f(path);
        f << "api_base=http://example:1234/v1\n";
        f << "api_key=sk-test\n";
        f << "kilo_balance_token=kilo-tok\n";
        f << "provider=openrouter\n";
        f << "model=my-model\n";
        f << "system_prompt=sys.md\n";
        f << "tools_prompt=tools.md\n";
        f << "git_prompt=git.md\n";
        f << "temperature=0.75\n";
        f << "max_tokens=4096\n";
        f << "stream=false\n";
        f << "thinking=off\n";
        f << "thinking_budget=1234\n";
        f << "reasoning_effort=high\n";
        f << "show_reasoning=false\n";
        f << "max_tool_iterations=7\n";
        f << "subagent_parallel=false\n";
        f << "subagent_max=2\n";
        f << "context_size=8192\n";
        f << "default_context_size=4096\n";
        f << "policy_approval=false\n";
        f << "detection_loop=true\n";
        f << "detection_duplicate=true\n";
        f << "wallet=false\n";
        f << "compression_threshold=0.5\n";
        f << "compression_min_turns=3\n";
        f << "compression_cooldown_turns=4\n";
        f << "compression_target_pct=12\n";
        f << "compression_keep_last_prompts=5\n";
        f << "experience_enabled=false\n";
        f << "experience_store_path=/tmp/exp.json\n";
        f << "experience_max_memories=11\n";
        f << "experience_max_skills=12\n";
        f << "experience_decay_rate=0.25\n";
        f << "experience_promote_threshold=13\n";
        f << "skills_interop=true\n";
        f << "skills_max_discovery=14\n";
        f << "skills_body_budget_tokens=15\n";
        f << "skills_index=http://mirror.example\n";
        f << "skills_install_scope=global\n";
        f << "skills_review_required=false\n";
        f << "skills_audit_required=true\n";
        f << "skills_update_review=false\n";
        f << "skills_update_check=never\n";
        f << "skills_index_token_env=INDEX_TOKEN\n";
        f << "skills_github_token_env=GH_TOKEN\n";
        f << "work_auto_attach=agents\n";
        f << "max_tool_result_bytes=65536\n";
        f << "log_path=/tmp/conv.jsonl\n";
        f << "debug_log=/tmp/dbg.log\n";
        f << "debug=1\n";
    }
    agent::Config c;
    c.load(path);

    ASSERT_EQ(c.api_base, "http://example:1234/v1");
    ASSERT_EQ(c.api_key, "sk-test");
    ASSERT_EQ(c.kilo_balance_token, "kilo-tok");
    ASSERT_EQ(c.provider_name, "openrouter");
    ASSERT_EQ(c.model, "my-model");
    ASSERT_TRUE(c.model_explicit);
    ASSERT_EQ(c.system_prompt_path, "sys.md");
    ASSERT_EQ(c.tools_prompt_path, "tools.md");
    ASSERT_EQ(c.git_prompt_path, "git.md");
    ASSERT_EQ(c.temperature, 0.75);
    ASSERT_EQ(c.max_tokens, 4096u);
    ASSERT_FALSE(c.stream);
    ASSERT_EQ(c.thinking, "off");
    ASSERT_EQ(c.thinking_budget, 1234);
    ASSERT_EQ(c.reasoning_effort, "high");
    ASSERT_FALSE(c.show_reasoning);
    ASSERT_EQ(c.max_tool_iterations, 7);
    ASSERT_FALSE(c.subagent_parallel);
    ASSERT_EQ(c.subagent_max, 2);
    ASSERT_EQ(c.context_size, 8192);
    ASSERT_TRUE(c.context_explicit);
    ASSERT_EQ(c.default_context_size, 4096);
    ASSERT_FALSE(c.policy_approval);
    ASSERT_TRUE(c.detection_loop);
    ASSERT_TRUE(c.detection_duplicate);
    ASSERT_FALSE(c.wallet_enabled);
    ASSERT_EQ(c.compression_threshold, 0.5);
    ASSERT_TRUE(c.compression_threshold_explicit);
    ASSERT_EQ(c.compression_min_turns, 3);
    ASSERT_TRUE(c.compression_min_turns_explicit);
    ASSERT_EQ(c.compression_cooldown_turns, 4);
    ASSERT_TRUE(c.compression_cooldown_turns_explicit);
    ASSERT_EQ(c.compression_target_pct, 12);
    ASSERT_TRUE(c.compression_target_pct_explicit);
    ASSERT_EQ(c.compression_keep_last_prompts, 5);
    ASSERT_TRUE(c.compression_keep_last_prompts_explicit);
    ASSERT_FALSE(c.experience_enabled);
    ASSERT_EQ(c.experience_store_path, "/tmp/exp.json");
    ASSERT_EQ(c.experience_max_memories, 11);
    ASSERT_EQ(c.experience_max_skills, 12);
    ASSERT_EQ(c.experience_decay_rate, 0.25);
    ASSERT_EQ(c.experience_promote_threshold, 13);
    ASSERT_TRUE(c.skills_interop);
    ASSERT_EQ(c.skills_max_discovery, 14);
    ASSERT_EQ(c.skills_body_budget_tokens, 15);
    ASSERT_EQ(c.skills_index, "http://mirror.example");
    ASSERT_EQ(c.skills_install_scope, "global");
    ASSERT_FALSE(c.skills_review_required);
    ASSERT_TRUE(c.skills_audit_required);
    ASSERT_FALSE(c.skills_update_review);
    ASSERT_EQ(c.skills_update_check, "never");
    ASSERT_EQ(c.skills_index_token_env, "INDEX_TOKEN");
    ASSERT_EQ(c.skills_github_token_env, "GH_TOKEN");
    ASSERT_EQ(c.work_auto_attach, "agents");
    ASSERT_EQ(c.max_tool_result_bytes, 65536);
    ASSERT_EQ(c.log_path, "/tmp/conv.jsonl");
    ASSERT_EQ(c.debug_log, "/tmp/dbg.log");
    ASSERT_TRUE(c.debug);
    ASSERT_TRUE(c.warnings.empty());
    std::remove(path.c_str());
}

TEST(config_acquisition_settings_validate_and_round_trip) {
    const std::string path = "/tmp/amber_acq_settings_test.conf";
    {
        std::ofstream f(path, std::ios::trunc);
        f << "skills_index=ftp://mirror\n";
        f << "skills_update_check=hourly\n";
        f << "skills_install_scope=everywhere\n";
        f << "skills_index=none\n"; // last write wins and is valid
        f << "skills_install_scope=global\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.skills_index, "none");
    ASSERT_EQ(c.skills_install_scope, "global");
    ASSERT_FALSE(c.warnings.empty());      // both bad values were reported
    ASSERT_TRUE(c.skills_review_required); // defaults survive

    c.skills_index = "https://mirror.example/api";
    c.skills_install_scope = "project";
    c.skills_review_required = false;
    c.skills_audit_required = true;
    c.skills_update_review = false;
    c.skills_update_check = "daily";
    const std::string saved = "/tmp/amber_acq_settings_saved.conf";
    ASSERT_TRUE(c.save_settings(saved));
    agent::Config d;
    d.load(saved);
    ASSERT_EQ(d.skills_index, "https://mirror.example/api");
    ASSERT_EQ(d.skills_install_scope, "project");
    ASSERT_FALSE(d.skills_review_required);
    ASSERT_TRUE(d.skills_audit_required);
    ASSERT_FALSE(d.skills_update_review);
    ASSERT_EQ(d.skills_update_check, "daily");
    ASSERT_TRUE(d.warnings.empty());
    std::remove(path.c_str());
    std::remove(saved.c_str());
}

// "1", "true" and "yes" are true; every other spelling (including empty) is
// false. Pins the accepted vocabulary so a new spelling cannot slip through.
TEST(config_load_bool_spellings) {
    const char* path = "/tmp/amber_cfg_bool.conf";
    struct Case {
        const char* value;
        bool expected;
    };
    const Case cases[] = {{"1", true},      {"true", true}, {"yes", true}, {"0", false},
                          {"false", false}, {"no", false},  {"", false}};
    for (const Case& tc : cases) {
        std::ofstream f(path);
        f << "stream=" << tc.value << "\n";
        f.close();
        agent::Config c; // stream defaults to true, so only an explicit false is visible
        c.load(path);
        ASSERT_EQ(c.stream, tc.expected);
    }
    std::remove(path);
}

// One bad value skips that key and warns; it never aborts the load or discards
// the rest of the file.
TEST(config_load_bad_number_warns_and_keeps_default) {
    std::string path = "/tmp/amber_cfg_badnum.conf";
    {
        std::ofstream f(path);
        f << "max_tool_iterations=abc\n";
        f << "temperature=hot\n";
        f << "thinking_budget=nope\n";
        f << "subagent_max=2\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.max_tool_iterations, 100); // default kept
    ASSERT_EQ(c.temperature, 0.2);         // default kept
    ASSERT_EQ(c.thinking_budget, -1);      // default kept
    ASSERT_EQ(c.subagent_max, 2);          // the good key still applied
    ASSERT_EQ(c.warnings.size(), 3u);
    std::remove(path.c_str());
}

// Blanks, comments, lines without '=' and unknown keys are all skipped quietly.
TEST(config_load_ignores_blanks_comments_and_malformed_lines) {
    std::string path = "/tmp/amber_cfg_noise.conf";
    {
        std::ofstream f(path);
        f << "\n";
        f << "# comment\n";
        f << "#model=commented-out\n";
        f << "no_equals_sign\n";
        f << "unknown_key=value\n";
        f << "model=kept\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "kept");
    ASSERT_TRUE(c.warnings.empty());
    std::remove(path.c_str());
}

// One layer of *matching* double quotes is stripped; single or unbalanced
// quotes are left alone. A lone quote strips to empty rather than throwing
// (substr(1, size-2) with size 1 is the empty string).
TEST(config_load_strips_matching_quotes_only) {
    std::string path = "/tmp/amber_cfg_quotes.conf";
    {
        std::ofstream f(path);
        f << "api_key=\"quoted\"\n";
        f << "model='single'\n";
        f << "log_path=\"unbalanced\n";
        f << "thinking=\"\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.api_key, "quoted");
    ASSERT_EQ(c.model, "'single'");
    ASSERT_EQ(c.log_path, "\"unbalanced");
    ASSERT_EQ(c.thinking, "");
    std::remove(path.c_str());
}

// The numeric edges: a negative unsigned is ignored, an empty optional int is
// ignored, and an explicit zero context stays "auto" (context_explicit false).
// None of the three warns — a skipped value is not a malformed one.
TEST(config_load_negative_and_empty_numbers) {
    std::string path = "/tmp/amber_cfg_nums.conf";
    {
        std::ofstream f(path);
        f << "max_tokens=-5\n";
        f << "default_context_size=\n";
        f << "context_size=0\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.max_tokens, 16384u); // default kept
    ASSERT_EQ(c.default_context_size, 0);
    ASSERT_EQ(c.context_size, 0);
    ASSERT_FALSE(c.context_explicit);
    ASSERT_TRUE(c.warnings.empty());
    std::remove(path.c_str());
}

// Mirrors what the TUI F10 "save settings" writes for a llama.cpp server, and
// that an optional (possibly empty) token survives a load round-trip. This
// guards the settings-persistence contract used by the TUI.
TEST(config_save_settings_roundtrip) {
    std::string path = "/tmp/amber_settings_test.conf";
    {
        std::ofstream f(path);
        f << "# amber settings\n";
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=sk-test-token\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.api_base, "http://localhost:8080/v1");
    ASSERT_EQ(c.api_key, "sk-test-token");
    ASSERT_EQ(c.model, "llama-3.2-3b-instruct");

    // Optional/blank token is also preserved as empty.
    {
        std::ofstream f(path);
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c2;
    c2.load(path);
    ASSERT_EQ(c2.api_key, "");
    std::remove(path.c_str());
}

TEST(config_save_settings_keeps_llm_global) {
    // The project-local settings file must NOT duplicate LLM provider config
    // (api_base / api_key / model / context_size); those stay in the global
    // config. save_settings() omits them by design.
    agent::Config c;
    c.api_base = "http://localhost:8080/v1";
    c.api_key = "sk-secret";
    c.model = "llama-3.2-3b";
    c.model_explicit = true;
    c.context_size = 8192;
    c.context_explicit = true;
    c.temperature = 0.7;
    c.stream = false;
    c.debug_log = "/tmp/amber_debug.log";

    std::string path = "/tmp/amber_settings_local.conf";
    ASSERT_TRUE(c.save_settings(path));

    agent::Config d;
    d.load(path);
    // LLM provider settings are intentionally omitted from the local file, so
    // loading it must NOT pick up the provider values we set above.
    ASSERT(d.api_base != "http://localhost:8080/v1");
    ASSERT(d.api_key != "sk-secret");
    ASSERT(d.model != "llama-3.2-3b");
    ASSERT(d.context_size != 8192);
    // Non-LLM settings round-trip.
    ASSERT_EQ(d.temperature, 0.7);
    ASSERT_EQ(d.stream, false);
    ASSERT_EQ(d.debug_log, "/tmp/amber_debug.log");
    // And the raw file must not contain the LLM keys.
    std::ifstream f(path);
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::fprintf(stderr, "[dbg] body(%zu): %.300s\n", body.size(), body.c_str());
    ASSERT(body.find("api_base=") == std::string::npos);
    std::fprintf(stderr, "[dbg] body(%zu): %.300s\n", body.size(), body.c_str());
    ASSERT(body.find("api_key=") == std::string::npos);
    std::fprintf(stderr, "[dbg] body(%zu): %.300s\n", body.size(), body.c_str());
    ASSERT(body.find("model=") == std::string::npos);
    std::remove(path.c_str());
}

TEST(config_missing_file_is_noop) {
    agent::Config c;
    c.model = "keep";
    c.load("/nonexistent/path/xyz.cfg");
    ASSERT_EQ(c.model, "keep");
}

// Regression: a saved config with a blank model / zero context must NOT mark
// those values explicit, otherwise startup auto-detection is permanently
// disabled and the server's real model/provider gets overwritten by defaults.
TEST(config_blank_model_and_zero_context_stay_auto) {
    std::string path = "/tmp/amber_auto_test.conf";
    {
        std::ofstream f(path);
        f << "api_base=http://localhost:8080/v1\n";
        f << "model=\n";         // blank => auto-detect
        f << "context_size=0\n"; // zero  => auto-detect
    }
    agent::Config c;
    c.load(path);
    ASSERT_TRUE(c.model.empty());
    ASSERT_FALSE(c.model_explicit);
    ASSERT_EQ(c.context_size, 0);
    ASSERT_FALSE(c.context_explicit);
    std::remove(path.c_str());
}

// An explicit (non-blank / positive) config still wins over auto-detection.
TEST(config_explicit_model_and_context_are_flagged) {
    std::string path = "/tmp/amber_explicit_test.conf";
    {
        std::ofstream f(path);
        f << "model=my-model\n";
        f << "context_size=16384\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_TRUE(c.model_explicit);
    ASSERT_EQ(c.context_size, 16384);
    ASSERT_TRUE(c.context_explicit);
    std::remove(path.c_str());
}

// Context size defaults to 0 (auto) so the gauge hides and probing is allowed.
TEST(config_context_default_is_auto) {
    agent::Config c;
    ASSERT_EQ(c.context_size, 0);
    ASSERT_FALSE(c.context_explicit);
}

// ---------------------------------------------------------------------------
// Provider presets and global/project settings tiers
// ---------------------------------------------------------------------------

namespace {

// The vendor presets are contributed by plugins now, so a provider test needs
// the same runtime the app builds at startup. The plugin state directory is
// redirected to a scratch tree: a developer who disabled a provider plugin in
// their own config must not change what these tests observe.
class PluginTestEnv {
public:
    explicit PluginTestEnv(const std::string& name = "env")
        : dir("/tmp/amber_plugin_test_" + name) {
        std::filesystem::remove_all(dir);
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
        runtime.add_bundled();
        runtime.start();
    }
    ~PluginTestEnv() {
        unsetenv("XDG_CONFIG_HOME");
        std::filesystem::remove_all(dir);
    }
    PluginTestEnv(const PluginTestEnv&) = delete;
    PluginTestEnv& operator=(const PluginTestEnv&) = delete;

    std::string dir;
    agent::Config cfg;
    agent::ToolRegistry tools;
    agent::Workspace ws;
    agent::PluginRuntime runtime{tools, cfg, ws};
};

} // namespace

TEST(config_provider_openrouter_preset) {
    PluginTestEnv env;
    auto svc = agent::make_default_provider_service(env.cfg);
    auto sel = svc->select("openrouter");
    ASSERT(sel.ok());
    ASSERT_EQ(sel.provider.api_base, "https://openrouter.ai/api/v1");
    ASSERT(!sel.provider.default_model.empty());
}

TEST(config_provider_kilocode_preset) {
    // Kilo's OpenAI-compatible gateway (docs: kilo.ai/docs/gateway). The
    // old api.kilocode.ai/v1 endpoint 404'd on every request.
    PluginTestEnv env;
    auto svc = agent::make_default_provider_service(env.cfg);
    auto sel = svc->select("kilocode");
    ASSERT(sel.ok());
    ASSERT_EQ(sel.provider.api_base, "https://api.kilo.ai/api/gateway");
    ASSERT(!sel.provider.default_model.empty());
}

TEST(config_provider_plugin_disabled_is_not_offered) {
    // The provider set follows plugin state: with the plugin off, its provider
    // is not in the list at all — and a provider file that still points at its
    // flavor is refused loudly rather than falling back to another protocol.
    PluginTestEnv env;
    ASSERT_TRUE(env.runtime.set_state("openrouter", false));

    auto svc = agent::make_default_provider_service(env.cfg);
    ASSERT_FALSE(svc->select("openrouter").ok());
    ASSERT_TRUE(svc->select("kilocode").ok()); // the others are unaffected
}

TEST(config_provider_unknown_is_loud_error) {
    auto svc = agent::make_default_provider_service(agent::Config{});
    auto sel = svc->select("nonexistent");
    ASSERT_FALSE(sel.ok()); // never a silent fallback
}

TEST(config_global_save_roundtrip_preserves_provider) {
    std::string path = "/tmp/amber_global_test.conf";
    agent::Config c;
    c.provider_name = "openrouter";
    c.api_base = "https://openrouter.ai/api/v1";
    c.api_key = "sk-test-123";
    ASSERT_TRUE(c.save_global(path));

    agent::Config d;
    d.load(path);
    ASSERT_EQ(d.provider_name, "openrouter");
    ASSERT_EQ(d.api_base, "https://openrouter.ai/api/v1");
    ASSERT_EQ(d.api_key, "sk-test-123");
    std::remove(path.c_str());
}

// An automatic resolution is not a user pin. save_global used to write
// context_size unconditionally, so a model switch turned the current window
// into a pin for the next start; the loader treats any value > 0 as explicit.
TEST(config_global_save_does_not_pin_an_automatic_window) {
    std::string path = "/tmp/amber_global_ctx.conf";
    agent::Config c;
    c.context_size = 262144;
    c.context_explicit = false;
    ASSERT_TRUE(c.save_global(path));

    agent::Config d;
    d.load(path);
    ASSERT_EQ(d.context_size, 0);
    ASSERT_FALSE(d.context_explicit);

    c.context_explicit = true;
    ASSERT_TRUE(c.save_global(path));
    agent::Config e;
    e.load(path);
    ASSERT_EQ(e.context_size, 262144);
    ASSERT_TRUE(e.context_explicit);
    std::remove(path.c_str());
}

TEST(config_global_save_roundtrip_kilo_balance_token) {
    std::string path = "/tmp/amber_global_kilo.conf";
    agent::Config c;
    c.kilo_balance_token = "kilo-account-oauth-token";
    ASSERT_TRUE(c.save_global(path));

    agent::Config d;
    d.load(path);
    ASSERT_EQ(d.kilo_balance_token, "kilo-account-oauth-token");
    std::remove(path.c_str());

    // No token configured -> the key is not written, load stays empty
    // (anonymous kilo use has no balance readout).
    std::string p2 = "/tmp/amber_global_kilo2.conf";
    agent::Config f;
    ASSERT_TRUE(f.save_global(p2));
    agent::Config g;
    g.load(p2);
    ASSERT_TRUE(g.kilo_balance_token.empty());
    std::remove(p2.c_str());
}

// Each tool declares the word the status bar shows while it runs, in the
// contribution that installs it. The display falls back to a generic word when
// one is missing, so a forgotten verb is invisible at runtime — which is
// exactly why it is asserted here against the real set.
TEST(tool_plugins_declare_their_display_verbs) {
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{});

    ASSERT_EQ(reg.meta_for("read").verb, std::string("reading"));
    ASSERT_EQ(reg.meta_for("write").verb, std::string("writing"));
    ASSERT_EQ(reg.meta_for("search").verb, std::string("searching"));
    ASSERT_EQ(reg.meta_for("bash").verb, std::string("hacking"));
    ASSERT_EQ(reg.meta_for("todowrite").verb, std::string("planning"));

    // The process set shares one contribution but names each tool.
    ASSERT_EQ(reg.meta_for("process_start").verb, std::string("spawning"));
    ASSERT_EQ(reg.meta_for("process_read").verb, std::string("reading"));
    ASSERT_EQ(reg.meta_for("process_stop").verb, std::string("stopping"));

    // Unknown tools are not an error: the display falls back, the registry
    // reports nothing rather than inventing a word.
    ASSERT(reg.meta_for("nonexistent").verb.empty());
}

// The balance readout belongs to the provider plugin that offers it: the core
// code has no idea the provider exists. The plugin resolves its own token
// (explicit override, else the gateway key when its provider is active) and
// reports only what it can honestly show.
TEST(kilocode_plugin_resolves_its_own_balance_token) {
    agent::Config cfg;

    // Another provider is active: the gateway key is not an account token.
    cfg.provider_name = "openrouter";
    cfg.api_key = "sk-openrouter";
    ASSERT_TRUE(agent::plugins::kilocode_balance_token(cfg).empty());

    // Its own provider: the gateway key doubles as the account token.
    cfg.provider_name = "kilocode";
    cfg.api_key = "kilo-jwt";
    ASSERT_EQ(agent::plugins::kilocode_balance_token(cfg), std::string("kilo-jwt"));

    // The explicit override wins regardless of the active provider.
    cfg.kilo_balance_token = "kilo-override";
    cfg.provider_name = "openrouter";
    ASSERT_EQ(agent::plugins::kilocode_balance_token(cfg), std::string("kilo-override"));

    // No key at all: nothing to fetch with.
    agent::Config anon;
    anon.provider_name = "kilocode";
    ASSERT_TRUE(agent::plugins::kilocode_balance_token(anon).empty());
}

// The provider's wire protocol travels with the provider itself. Nothing in
// the core looks a provider's dialect up by name any more.
TEST(apply_selection_copies_flavor_from_the_provider) {
    setenv("XDG_CONFIG_HOME", "/tmp/amber_xdg_caps", 1);
    std::filesystem::remove_all("/tmp/amber_xdg_caps");

    agent::Config cfg;
    agent::ToolRegistry tools;
    agent::Workspace ws;
    agent::PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();

    auto svc = agent::make_default_provider_service(agent::Config{});

    auto kilo = svc->select("kilocode");
    ASSERT(kilo.ok());
    agent::Config kilo_cfg;
    agent::apply_selection(kilo_cfg, kilo);
    ASSERT_EQ(kilo_cfg.flavor, "openai"); // the gateway speaks openai

    auto gemini = svc->select("gemini");
    ASSERT(gemini.ok());
    agent::Config gemini_cfg;
    agent::apply_selection(gemini_cfg, gemini);
    ASSERT_EQ(gemini_cfg.flavor, "gemini"); // served by the plugin's preset

    std::filesystem::remove_all("/tmp/amber_xdg_caps");
    unsetenv("XDG_CONFIG_HOME");
}

// The derived flavor is never persisted: it is recomputed from the provider on
// every boot, so a stale value cannot survive in a config file.
TEST(flavor_is_derived_and_not_persisted) {
    std::string path = "/tmp/amber_flavor_persist.conf";
    agent::Config c;
    c.flavor = "openai";
    ASSERT_TRUE(c.save_global(path));

    agent::Config back;
    back.load(path);
    ASSERT_EQ(back.flavor, "openai"); // default, not stored
    std::remove(path.c_str());
}

TEST(provider_service_missing_key_is_warning) {
    // The key-required policy lives in the domain: select() warns, never
    // fails, when a key-requiring provider has no key (the user may set
    // one via env / F10 without switching again).
    PluginTestEnv env;
    auto svc = agent::make_default_provider_service(env.cfg);
    auto sel = svc->select("openrouter");
    ASSERT(sel.ok());
    ASSERT(sel.warning.find("API key") != std::string::npos);
}

TEST(provider_builtin_key_save_clears_warning) {
    // The /set provider inline key prompt persists a key for a built-in
    // provider (kilocode/openrouter) into its <name>.conf, which the file
    // repo (later) overlays over the static preset. After the save the
    // requires-key warning must be gone — switching must not re-prompt.
    setenv("XDG_CONFIG_HOME", "/tmp/amber_xdg_builtin_key", 1);
    std::filesystem::remove_all("/tmp/amber_xdg_builtin_key");
    agent::Config cfg;
    agent::ToolRegistry tools;
    agent::Workspace ws;
    agent::PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();
    {
        auto svc = agent::make_default_provider_service(cfg);
        auto sel = svc->select("kilocode");
        ASSERT(sel.ok());
        ASSERT(sel.provider.builtin);
        ASSERT(sel.provider.requires_key);
        ASSERT(sel.warning.find("API key") != std::string::npos);

        // The TUI saves the provider with the user-entered key, keeping the
        // builtin marker so the preset's endpoint/model defaults survive.
        agent::Provider k = sel.provider;
        k.api_key = "kilo-sk-test";
        ASSERT(svc->save(k));
    }
    {
        // Fresh service (restart): the file repo now carries the key.
        auto svc2 = agent::make_default_provider_service(cfg);
        auto sel2 = svc2->select("kilocode");
        ASSERT(sel2.ok());
        ASSERT_EQ(sel2.provider.api_key, "kilo-sk-test");
        ASSERT_EQ(sel2.provider.api_base, "https://api.kilo.ai/api/gateway");
        ASSERT(sel2.warning.empty()); // no re-prompt after restart
    }
    std::filesystem::remove_all("/tmp/amber_xdg_builtin_key");
    unsetenv("XDG_CONFIG_HOME");
}

TEST(config_validate_skips_api_key_for_custom) {
    agent::Config c;
    c.api_key.clear();
    auto errs = c.validate();
    bool found = false;
    for (const auto& e : errs)
        if (e.find("api_key") != std::string::npos)
            found = true;
    ASSERT_FALSE(found);
}

TEST(config_project_settings_omits_provider_fields) {
    // save_settings() must NOT write api_base / api_key / model / provider
    agent::Config c;
    c.api_base = "http://localhost:8080/v1";
    c.api_key = "sk-secret";
    c.model = "llama-3.2-3b";
    c.provider_name = "openrouter";
    c.temperature = 0.7;

    std::string path = "/tmp/amber_project_test.conf";
    ASSERT_TRUE(c.save_settings(path));

    // Load into a fresh config
    agent::Config d;
    d.load(path);
    // Provider fields should still be defaults (not overwritten by project file)
    ASSERT(d.api_base != "http://localhost:8080/v1");
    ASSERT(d.api_key != "sk-secret");
    ASSERT(d.provider_name != "openrouter");
    // Project fields should be loaded
    ASSERT_EQ(d.temperature, 0.7);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// UTF-8 safety: tool/model text may carry invalid bytes (e.g. binary from
// grep). Serializing it for the API request used to throw
// [json.exception.type_error.316] and abort the turn. It must not.
// ---------------------------------------------------------------------------

TEST(request_body_survives_invalid_utf8) {
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message tool;
    tool.role = "tool";
    tool.name = "search";
    tool.tool_call_id = "x";
    // 0x66 ('f') followed by a lone continuation byte 0x80 — invalid UTF-8.
    std::string bad = "hits:\nfoo";
    bad.push_back(static_cast<char>(0x80)); // lone continuation byte: invalid UTF-8
    bad += "bar";
    tool.content = bad;
    msgs.push_back(tool);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    std::string payload;
    bool threw = false;
    try {
        payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (...) {
        threw = true;
    }
    ASSERT_FALSE(threw);
    ASSERT(payload.find("\xEF\xBF\xBD") != std::string::npos); // U+FFFD present
}

TEST(request_builder_merges_consecutive_system_messages) {
    // Strict GGUF chat templates (e.g. Qwen 3.6 dense) raise HTTP 500 when the
    // request carries more than one system message ("System message must be at
    // the beginning"). The memory/skills blocks are injected as a second
    // role=system message, so the wire format must merge consecutive system
    // messages into one. Token-level KV prefix caching is unaffected: the
    // common prefix tokens stay identical either way.
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message s1;
    s1.role = "system";
    s1.content = "main prompt";
    msgs.push_back(s1);
    agent::Message s2;
    s2.role = "system";
    s2.content = "memories block";
    msgs.push_back(s2);
    agent::Message u;
    u.role = "user";
    u.content = "hi";
    msgs.push_back(u);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    const auto& wire = body["messages"];
    ASSERT_EQ(wire.size(), 2u);
    ASSERT_EQ(wire[0]["role"], "system");
    ASSERT_EQ(wire[0]["content"], "main prompt\n\nmemories block");
    ASSERT_EQ(wire[1]["role"], "user");

    // A single system message is passed through untouched.
    std::vector<agent::Message> single;
    single.push_back(s1);
    json body2 = agent::make_dialect("openai")->build_chat_body(c, single, no_tools, false);
    ASSERT_EQ(body2["messages"].size(), 1u);
    ASSERT_EQ(body2["messages"][0]["content"], "main prompt");
}

TEST(request_builder_hoists_midstream_system_into_leading_block) {
    // The compressed-context archive is a role=system message that sits AFTER
    // the conversation turns (apply_classification appends it, then prepends
    // the real system prompt). A strict GGUF template rejects a system message
    // that is not at the start (HTTP 500 "System message must be at the
    // beginning"). Every system message — wherever it appears — must merge into
    // ONE block emitted BEFORE the first non-system message.
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message s1;
    s1.role = "system";
    s1.content = "main prompt";
    msgs.push_back(s1);
    agent::Message u;
    u.role = "user";
    u.content = "hi";
    msgs.push_back(u);
    agent::Message a;
    a.role = "assistant";
    a.content = "hello";
    msgs.push_back(a);
    // Trailing system: the compressed-context archive after the turns.
    agent::Message arch;
    arch.role = "system";
    arch.content = "Compressed conversation context:\n{\"archive\":[]}";
    msgs.push_back(arch);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    const auto& wire = body["messages"];
    // Exactly one system message, at the front, carrying BOTH system contents.
    size_t system_count = 0;
    for (const auto& m : wire)
        if (m["role"] == "system")
            ++system_count;
    ASSERT_EQ(system_count, 1u);
    ASSERT_EQ(wire[0]["role"], "system");
    std::string sys = wire[0]["content"];
    ASSERT(sys.find("main prompt") != std::string::npos);
    ASSERT(sys.find("Compressed conversation context") != std::string::npos);
    // Original turn order preserved after the single leading system block.
    ASSERT_EQ(wire[1]["role"], "user");
    ASSERT_EQ(wire[2]["role"], "assistant");
}

TEST(request_builder_assistant_message_always_has_content) {
    // Regression: a reasoning model can answer with content "" and the whole
    // reply in reasoning_content. Serializing that as {"role":"assistant"}
    // (no content field) makes the server reject the request with HTTP 400
    // ("Assistant message must contain either 'content' or 'tool_calls'").
    // build_chat_body must always emit a content field (even empty) so a
    // stripped/empty reply never produces a malformed request.
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message u;
    u.role = "user";
    u.content = "think hard";
    msgs.push_back(u);
    agent::Message a;
    a.role = "assistant";
    a.content = ""; // empty, no tool_calls
    msgs.push_back(a);
    agent::Message t;
    t.role = "tool";
    t.name = "read";
    t.tool_call_id = "c1";
    t.content = ""; // empty tool result
    msgs.push_back(t);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    ASSERT(body.contains("messages"));
    for (auto& m : body["messages"]) {
        // every message must carry a content field (regression: empty
        // assistant/tool content previously dropped the field -> HTTP 400).
        ASSERT(m.contains("content"));
    }
}

// Legacy/corrupt history must never reach the wire: a restored session file
// can carry a name-less "{}" placeholder tool_call (a sparse-index stream on
// an older build), and a strict gateway rejects the whole request with
// "Invalid discriminator value ... tool_calls.0.type". build_chat_body drops
// placeholders and defaults a missing `type` to "function".
TEST(request_builder_sanitizes_placeholder_tool_calls) {
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message u;
    u.role = "user";
    u.content = "hi";
    msgs.push_back(u);
    agent::Message a;
    a.role = "assistant";
    a.tool_calls = json::array({
        json::object(), // <-- the corrupt placeholder (messages.7 case)
        {{"function", {{"name", "bash"}, {"arguments", R"({"command":"ls -loha"})"}}},
         {"id", "call_1"}}, // type missing -> defaulted
        {{"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"a.txt"})"}}},
         {"id", "call_2"}} // fully valid, unchanged
    });
    msgs.push_back(a);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    const auto& wire = body["messages"][1]["tool_calls"];
    ASSERT_EQ(wire.size(), 2u);
    ASSERT_EQ(wire[0]["function"]["name"], "bash");
    ASSERT_EQ(wire[0]["type"], "function"); // defaulted, not absent
    ASSERT_EQ(wire[1]["id"], "call_2");
    ASSERT_EQ(wire[1]["type"], "function");
    // No element is an empty placeholder.
    for (const auto& tc : wire) {
        ASSERT(tc.contains("function"));
        ASSERT(tc["function"].contains("name"));
        ASSERT(!tc["function"]["name"].get<std::string>().empty());
    }
}

// When EVERY tool_call in an assistant message is a placeholder, the wire
// message degrades to plain content — never an empty tool_calls array.
TEST(request_builder_all_placeholder_tool_calls_degrades) {
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message a;
    a.role = "assistant";
    a.content = "I tried but nothing ran.";
    a.tool_calls = json::array({json::object(), json::object()});
    msgs.push_back(a);

    std::vector<std::shared_ptr<agent::Tool>> no_tools;
    json body = agent::make_dialect("openai")->build_chat_body(c, msgs, no_tools, false);
    const auto& m = body["messages"][0];
    ASSERT_FALSE(m.contains("tool_calls"));
    ASSERT_EQ(m["content"], "I tried but nothing ran.");
}

// The buffered (non-streaming) ingestion path applies the same sanitizer, so
// junk from a server response never enters the context stack either.
TEST(message_from_completion_sanitizes_tool_calls) {
    std::string body =
        R"({"choices":[{"message":{)"
        R"("role":"assistant","content":null,"tool_calls":[)"
        R"({},)"
        R"({"function":{"name":"read","arguments":"{\"path\":\"a.txt\"}"},"id":"c1"})"
        R"(]}}]})";
    agent::Message m = agent::make_dialect("openai")->parse_completion(body);
    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "read");
    ASSERT_EQ(m.tool_calls[0]["type"], "function"); // defaulted
}

// ---------------------------------------------------------------------------
// Tool registry
// ---------------------------------------------------------------------------

TEST(registry_register_and_find) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(r, jobs, todos);
    ASSERT_FALSE(r.empty());
    // The tool-plugin tools plus the three skill tools, which arrive
    // through the same install path (one definition, two paths).
    ASSERT_EQ(r.snapshot_tools().size(), 15u);
    ASSERT(r.find("read") != nullptr);
    ASSERT(r.find("write") != nullptr);
    ASSERT(r.find("search") != nullptr);
    ASSERT(r.find("bash") != nullptr);
    ASSERT(r.find("process_start") != nullptr);
    ASSERT(r.find("process_read") != nullptr);
    ASSERT(r.find("process_stop") != nullptr);
    ASSERT(r.find("nonexistent") == nullptr);
}

// ---------------------------------------------------------------------------
// Provider domain (hexagonal: service + fake adapters)
// ---------------------------------------------------------------------------

class FakeProviderRepo : public agent::ProviderRepository {
public:
    std::vector<agent::Provider> items;
    std::vector<agent::Provider> saved;
    bool save_ret = true;

    std::vector<agent::Provider> all() const override { return items; }
    std::optional<agent::Provider> find(const std::string& n) const override {
        for (const auto& p : items)
            if (p.name == n)
                return p;
        return std::nullopt;
    }
    bool save(const agent::Provider& p) override {
        saved.push_back(p);
        return save_ret;
    }
    bool remove(const std::string& n) override {
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const agent::Provider& p) { return p.name == n; }),
                    items.end());
        return true;
    }
};

std::vector<std::unique_ptr<agent::ProviderRepository>>
make_repos(std::unique_ptr<agent::ProviderRepository> repo) {
    std::vector<std::unique_ptr<agent::ProviderRepository>> v;
    v.push_back(std::move(repo));
    return v;
}

class FakeModelCatalog : public agent::ModelCatalog {
public:
    std::vector<std::string> list_models(const agent::Provider&) const override {
        return {"m1", "m2"};
    }
};

agent::Provider test_provider(const std::string& name, const std::string& base,
                              const std::string& model = "") {
    agent::Provider p;
    p.name = name;
    p.api_base = base;
    p.api_key = "sk-" + name;
    p.default_model = model;
    return p;
}

TEST(provider_service_select_applies_connection) {
    auto repo = std::make_unique<FakeProviderRepo>();
    repo->items.push_back(test_provider("alpha", "https://alpha.test/v1"));
    auto svc =
        agent::ProviderService(make_repos(std::move(repo)), std::make_unique<FakeModelCatalog>());

    auto sel = svc.select("alpha");
    ASSERT(sel.ok());
    ASSERT_EQ(sel.provider.api_base, "https://alpha.test/v1");
    ASSERT_EQ(sel.provider.api_key, "sk-alpha");

    agent::Config cfg;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.provider_name, "alpha");
    ASSERT_EQ(cfg.api_base, "https://alpha.test/v1");
    ASSERT_EQ(cfg.api_key, "sk-alpha");
}

TEST(apply_selection_scopes_the_context_window) {
    auto repo = std::make_unique<FakeProviderRepo>();
    repo->items.push_back(test_provider("alpha", "https://alpha.test/v1"));
    auto svc =
        agent::ProviderService(make_repos(std::move(repo)), std::make_unique<FakeModelCatalog>());
    auto sel = svc.select("alpha");
    ASSERT(sel.ok());

    agent::Config cfg;
    cfg.context_size = 131072;
    cfg.context_explicit = false;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.context_size, 0); // no provider default: unknown, not the old value

    cfg.context_explicit = true;
    cfg.context_size = 200000;
    sel.provider.default_context_size = 524288;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.context_size, 200000); // a user pin survives

    cfg.context_explicit = false;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.context_size, 524288); // the provider default is adopted

    // Capabilities belong to the listing, not the previous provider.
    cfg.model_max_output = 8192;
    cfg.model_supports_reasoning = true;
    cfg.model_supported_parameters = {"reasoning"};
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.model_max_output, 0);
    ASSERT_FALSE(cfg.model_supports_reasoning);
    ASSERT(cfg.model_supported_parameters.empty());
}

TEST(openai_request_respects_output_and_reasoning) {
    auto d = agent::make_dialect("openai");
    agent::Config cfg;
    cfg.model = "vendor/model";
    cfg.max_tokens = 16384;
    cfg.model_max_output = 4096;
    std::vector<agent::Message> msgs;

    auto body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["max_tokens"].get<int>(), 4096);

    cfg.model_supports_reasoning = true;
    cfg.reasoning_effort = "high";
    cfg.model_supported_parameters = {"reasoning"};
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT(body.contains("reasoning"));
    ASSERT(!body.contains("reasoning_effort"));
    ASSERT_EQ(body["reasoning"]["effort"].get<std::string>(), "high");

    cfg.model_supported_parameters = {"reasoning_effort"};
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "high");
    ASSERT(!body.contains("reasoning"));

    cfg.model_supported_parameters.clear();
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT(!body.contains("reasoning_effort"));
    ASSERT(!body.contains("reasoning"));

    // Tool support is advertised too: an explicit list without "tools" means
    // no tool schema; an empty (unknown) list keeps the tools.
    class ProbeTool : public agent::Tool {
    public:
        std::string name() const noexcept override { return "probe"; }
        std::string description() const noexcept override { return "probe"; }
        agent::json parameters_schema() const override { return agent::json::object(); }
        agent::ToolResult execute(const agent::json&) const override { return {true, "ok", ""}; }
    };
    std::vector<std::shared_ptr<agent::Tool>> tools{std::make_shared<ProbeTool>()};
    cfg.model_supported_parameters = {"reasoning"};
    body = d->build_chat_body(cfg, msgs, tools, false);
    ASSERT(!body.contains("tools"));
    cfg.model_supported_parameters = {"tools"};
    body = d->build_chat_body(cfg, msgs, tools, false);
    ASSERT(body.contains("tools"));
    cfg.model_supported_parameters.clear();
    body = d->build_chat_body(cfg, msgs, tools, false);
    ASSERT(body.contains("tools"));
}

TEST(openai_model_list_carries_capabilities) {
    auto d = agent::make_dialect("openai");
    const std::string body = R"({"data":[{"id":"vendor/model","context_length":128000,)"
                             R"("top_provider":{"max_completion_tokens":4096},)"
                             R"("supported_parameters":["reasoning","tools"]}]})";
    auto info = d->parse_models_response(body);
    ASSERT_EQ(info.max_output, 4096);
    ASSERT(info.reasoning);
    ASSERT_EQ(info.supported_parameters.size(), 2u);

    agent::Config cfg;
    agent::merge_server_info(cfg, info);
    ASSERT_EQ(cfg.model_max_output, 4096);
    ASSERT(cfg.model_supports_reasoning);
    ASSERT_EQ(cfg.model_supported_parameters.size(), 2u);
}

TEST(provider_service_select_unknown_fails) {
    auto svc = agent::ProviderService(make_repos(std::make_unique<FakeProviderRepo>()),
                                      std::make_unique<FakeModelCatalog>());
    auto sel = svc.select("does-not-exist");
    ASSERT_FALSE(sel.ok());
    ASSERT(sel.error.find("unknown provider") != std::string::npos);
}

TEST(provider_service_select_unconfigured_fails) {
    // A provider with no endpoint (the unconfigured-custom case) is a
    // loud error, never a silent fallback to another provider.
    auto repo = std::make_unique<FakeProviderRepo>();
    repo->items.push_back(test_provider("custom", ""));
    auto svc =
        agent::ProviderService(make_repos(std::move(repo)), std::make_unique<FakeModelCatalog>());
    auto sel = svc.select("custom");
    ASSERT_FALSE(sel.ok());
    ASSERT(sel.error.find("no endpoint") != std::string::npos);
}

TEST(provider_service_auto_loads_last_model) {
    auto repo = std::make_unique<FakeProviderRepo>();
    repo->items.push_back(test_provider("alpha", "https://alpha.test/v1", "alpha-last-model"));
    auto svc =
        agent::ProviderService(make_repos(std::move(repo)), std::make_unique<FakeModelCatalog>());
    auto sel = svc.select("alpha");
    agent::Config cfg;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.model, "alpha-last-model");
    ASSERT(cfg.model_explicit);
}

TEST(provider_service_remember_model_persists) {
    auto repo = std::make_unique<FakeProviderRepo>();
    repo->items.push_back(test_provider("alpha", "https://alpha.test/v1"));
    auto* raw = repo.get();
    auto svc =
        agent::ProviderService(make_repos(std::move(repo)), std::make_unique<FakeModelCatalog>());
    ASSERT(svc.remember_model("alpha", "new-model"));
    ASSERT_EQ(raw->saved.size(), 1u);
    ASSERT_EQ(raw->saved[0].name, "alpha");
    ASSERT_EQ(raw->saved[0].default_model, "new-model");
}

TEST(provider_service_available_merges_and_dedups) {
    auto repo1 = std::make_unique<FakeProviderRepo>();
    repo1->items.push_back(test_provider("shared", "https://one.test/v1"));
    auto repo2 = std::make_unique<FakeProviderRepo>();
    repo2->items.push_back(test_provider("shared", "https://two.test/v1"));
    repo2->items.push_back(test_provider("unique", "https://u.test/v1"));
    std::vector<std::unique_ptr<agent::ProviderRepository>> repos;
    repos.push_back(std::move(repo1));
    repos.push_back(std::move(repo2));
    auto svc = agent::ProviderService(std::move(repos), std::make_unique<FakeModelCatalog>());
    auto all = svc.available();
    size_t shared = 0;
    for (const auto& p : all)
        if (p.name == "shared")
            shared++;
    ASSERT_EQ(shared, 1u); // de-duplicated
    ASSERT_EQ(all.size(), 2u);
    // Later repository wins on collision.
    for (const auto& p : all)
        if (p.name == "shared")
            ASSERT_EQ(p.api_base, "https://two.test/v1");
}

TEST(provider_custom_is_a_plugin_preset) {
    // Custom follows the same lifecycle as every other provider: contributed
    // by its plugin, selectable, and loud+actionable when unconfigured — never
    // a silent fallback, never missing.
    PluginTestEnv env("custom_preset");
    auto svc = agent::make_default_provider_service(env.cfg);
    bool found = false;
    for (const auto& p : svc->available())
        if (p.name == "custom") {
            found = true;
            ASSERT(p.builtin);
            ASSERT_TRUE(p.api_base.empty()); // the file supplies it
        }
    ASSERT(found);
    auto sel = svc->select("custom");
    ASSERT_FALSE(sel.ok());
    ASSERT(sel.error.find("no endpoint") != std::string::npos);
}

TEST(provider_custom_file_override_wins) {
    // The dedicated file (later repo) overrides the plugin's preset.
    PluginTestEnv env("custom_override");
    agent::Config conn;
    conn.api_base = "https://custom.test/v1";
    conn.api_key = "sk-custom";
    conn.model = "custom-model";
    ASSERT(agent::seed_provider("custom", conn));

    auto svc = agent::make_default_provider_service(env.cfg);
    auto sel = svc->select("custom");
    ASSERT(sel.ok());
    ASSERT_EQ(sel.provider.api_base, "https://custom.test/v1");
    ASSERT_EQ(sel.provider.api_key, "sk-custom");
    ASSERT_EQ(sel.provider.default_model, "custom-model");

    // Removing the file restores the unconfigured preset.
    std::filesystem::remove(agent::global_config_dir() + "/providers/custom.conf");
    auto svc2 = agent::make_default_provider_service(env.cfg);
    auto sel2 = svc2->select("custom");
    ASSERT_FALSE(sel2.ok());
}

TEST(provider_seed_writes_all_keys) {
    PluginTestEnv env("custom_seed");
    agent::Config conn;
    conn.api_base = "https://seed.test/v1";
    conn.api_key = "sk-seed";
    conn.model = "seed-model";
    conn.context_size = 12345;
    ASSERT(agent::seed_provider("custom", conn));
    const std::string path = agent::global_config_dir() + "/providers/custom.conf";
    std::ifstream f(path);
    ASSERT(f.good());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT(content.find("provider=custom") != std::string::npos);
    ASSERT(content.find("api_base=https://seed.test/v1") != std::string::npos);
    ASSERT(content.find("api_key=sk-seed") != std::string::npos);
    ASSERT(content.find("default_model=seed-model") != std::string::npos);
    ASSERT(content.find("default_context_size=12345") != std::string::npos);

    // An empty connection still writes a complete template.
    agent::Config empty;
    empty.api_base.clear();
    ASSERT(agent::seed_provider("custom", empty));

    // The helper is name-agnostic: any provider the user configures writes its
    // own file, and an empty name is refused rather than writing "*.conf".
    ASSERT_TRUE(agent::seed_provider("my-endpoint", conn));
    ASSERT_TRUE(
        std::filesystem::exists(agent::global_config_dir() + "/providers/my-endpoint.conf"));
    ASSERT_FALSE(agent::seed_provider("", conn));
}

// The architecture claim, made testable: the core declares no providers of its
// own. With no plugins registered, only what the user wrote is offered.
TEST(core_declares_no_providers_without_plugins) {
    setenv("XDG_CONFIG_HOME", "/tmp/amber_xdg_no_plugins", 1);
    std::filesystem::remove_all("/tmp/amber_xdg_no_plugins");

    auto svc = agent::make_default_provider_service(agent::Config{});
    ASSERT_TRUE(svc->available().empty());
    ASSERT_FALSE(svc->select("custom").ok());
    ASSERT_FALSE(svc->select("openrouter").ok());
    ASSERT_FALSE(svc->select("anthropic").ok());

    std::filesystem::remove_all("/tmp/amber_xdg_no_plugins");
    unsetenv("XDG_CONFIG_HOME");
}

// ...and a disabled provider plugin removes exactly its own provider.
TEST(disabling_a_provider_plugin_removes_only_that_provider) {
    PluginTestEnv env("provider_disable");
    ASSERT_TRUE(env.runtime.set_state("custom", false));
    auto svc = agent::make_default_provider_service(env.cfg);
    ASSERT_FALSE(svc->select("custom").ok());
    ASSERT_TRUE(svc->select("openrouter").ok());
    ASSERT_TRUE(svc->select("gemini").ok());

    // Re-enabling brings the provider back. It is unconfigured again, so
    // selection reports the missing endpoint rather than "unknown provider" —
    // the preset is present, which is the point.
    ASSERT_TRUE(env.runtime.set_state("custom", true));
    auto svc2 = agent::make_default_provider_service(env.cfg);
    auto sel2 = svc2->select("custom");
    ASSERT_FALSE(sel2.ok());
    ASSERT(sel2.error.find("unknown provider") == std::string::npos);
    ASSERT(sel2.error.find("no endpoint") != std::string::npos);
}

TEST(file_provider_repository_roundtrip) {
    const std::string dir = agent::global_config_dir() + "/providers";
    std::filesystem::create_directories(dir);
    auto repo = agent::make_file_provider_repository();
    ASSERT(repo != nullptr);
    agent::Provider p = test_provider("zzz_roundtrip", "https://roundtrip.test/v1", "last-model");
    p.requires_key = true;
    ASSERT(repo->save(p));
    auto found = repo->find("zzz_roundtrip");
    ASSERT(found.has_value());
    ASSERT_EQ(found->api_base, "https://roundtrip.test/v1");
    ASSERT_EQ(found->api_key, "sk-zzz_roundtrip");
    ASSERT_EQ(found->default_model, "last-model");
    ASSERT(found->requires_key);
    ASSERT(repo->remove("zzz_roundtrip"));
    ASSERT_FALSE(repo->find("zzz_roundtrip").has_value());
}

TEST(registry_repeated_registration_dedups) {
    // A second Agent (new window/session) re-registers skill tools into
    // the shared registry; duplicate names in the tools[] schema are
    // rejected by strict servers ("Tool names must be unique").
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(r, jobs, todos);
    const size_t first = r.snapshot_tools().size();
    agent::register_default_tools(r, jobs, todos);
    ASSERT_EQ(r.snapshot_tools().size(), first); // no duplicates
    std::set<std::string> names;
    for (const auto& t : r.snapshot_tools())
        names.insert(t->name());
    ASSERT_EQ(names.size(), first); // schema names unique
}

// The OpenAI tools[] payload is the dialect's responsibility, built from the
// registry's tool metadata — the registry itself stays protocol-agnostic (see
// docs/spec/llm-client/dialect.md).
TEST(dialect_openai_builds_tool_payload) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(r, jobs, todos);

    agent::Config cfg;
    std::vector<agent::Message> msgs;
    agent::Message user;
    user.role = "user";
    user.content = "hi";
    msgs.push_back(user);

    agent::json body =
        agent::make_dialect("openai")->build_chat_body(cfg, msgs, r.snapshot_tools(), false);
    const agent::json& s = body["tools"];
    ASSERT(s.is_array());
    ASSERT_EQ(s.size(), 15u);
    for (const auto& t : s) {
        ASSERT(t.contains("type"));
        ASSERT_EQ(t["type"], "function");
        ASSERT(t["function"].contains("name"));
        ASSERT(t["function"].contains("description"));
        ASSERT(t["function"]["parameters"].contains("properties"));
    }
    ASSERT_EQ(body["tool_choice"], "auto");
}

// ---------------------------------------------------------------------------
// Prompt loading + tool advertising
// ---------------------------------------------------------------------------

TEST(prompt_missing_file_empty) {
    ASSERT_EQ(agent::load_prompt("/does/not/exist.md"), "");
}

TEST(prompt_loads_existing) {
    std::string path = "/tmp/amber_prompt_test.md";
    {
        std::ofstream f(path);
        f << "# Title\nbody text\n";
    }
    ASSERT_EQ(agent::load_prompt(path), "# Title\nbody text\n");
    std::remove(path.c_str());
}

TEST(prompt_render_tools_markdown_lists_all) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(r, jobs, todos);
    std::string md = agent::render_tools_markdown(r);
    ASSERT(md.find("# Tools") != std::string::npos);
    ASSERT(md.find("`read`") != std::string::npos);
    ASSERT(md.find("`write`") != std::string::npos);
    ASSERT(md.find("`search`") != std::string::npos);
    ASSERT(md.find("`bash`") != std::string::npos);
    ASSERT(md.find("path") != std::string::npos); // a known parameter
}

// The ordering authority: every injected-block priority lives in one table
// (include/agent/prompt_priorities.h). The header's static_asserts already refuse
// a collision at build time; these tests name the offending row when one slips
// through and pin the band order the assembly depends on.
TEST(prompt_priorities_sit_inside_their_band_and_do_not_collide) {
    std::string violations;
    std::set<int> seen;
    for (const agent::PromptPriorityEntry& entry : agent::kPromptPriorities) {
        const int begin = agent::prompt_band_begin(entry.band);
        const int end = agent::prompt_band_end(entry.band);
        if (entry.value < begin || entry.value >= end) {
            violations += std::string(entry.name) + "=" + std::to_string(entry.value) +
                          " is outside [" + std::to_string(begin) + "," + std::to_string(end) +
                          "); ";
        }
        if (!seen.insert(entry.value).second)
            violations +=
                std::string(entry.name) + " collides on " + std::to_string(entry.value) + "; ";
    }
    ASSERT_EQ(violations, "");
    ASSERT_EQ(seen.size(), agent::prompt_priority_count());
}

TEST(prompt_bands_are_contiguous_and_ordered_by_owner) {
    // Head blocks read as instructions before the conversation, project facts and
    // tool docs follow, then the tail blocks, then plugin contributions. A gap or
    // a reordering here changes what the model reads when, so it is pinned.
    const auto& order = agent::kPromptBands;
    for (std::size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i) {
        ASSERT_TRUE(agent::prompt_band_begin(order[i]) < agent::prompt_band_begin(order[i + 1]));
        ASSERT_EQ(agent::prompt_band_end(order[i]), agent::prompt_band_begin(order[i + 1]));
    }
    ASSERT_TRUE(agent::prompt_band_begin(order[0]) > 0);
    ASSERT_EQ(agent::prompt_band_end(agent::PromptBand::Plugin), 1100);
}

// ---------------------------------------------------------------------------
// read tool (pagination)
// ---------------------------------------------------------------------------

TEST(read_tool_basic_and_pagination) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_read_test.txt";
    {
        std::ofstream f(path);
        for (int i = 1; i <= 10; ++i)
            f << "line " << i << "\n";
    }
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", path}, {"offset", 1}, {"limit", 3}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("1:\tline 1") != std::string::npos);
    ASSERT(r.output.find("3:\tline 3") != std::string::npos);
    ASSERT(r.output.find("remaining") != std::string::npos);

    // page 2
    auto r2 = tool->execute({{"path", path}, {"offset", 4}, {"limit", 3}});
    ASSERT_TRUE(r2.ok);
    ASSERT(r2.output.find("4:\tline 4") != std::string::npos);
    ASSERT(r2.output.find("6:\tline 6") != std::string::npos);

    // past end reports EOF, no remaining
    auto r3 = tool->execute({{"path", path}, {"offset", 9}, {"limit", 50}});
    ASSERT_TRUE(r3.ok);
    ASSERT(r3.output.find("end of file") != std::string::npos);
    std::remove(path.c_str());
}

TEST(read_tool_missing_path_errors) {
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"limit", 5}}); // no path
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.error.empty());
}

TEST(read_tool_cannot_open_error_is_relative) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_missing_rel88.txt";
    std::remove(path.c_str());
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", path}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("/tmp/amber_missing_rel88.txt") == std::string::npos);
    ASSERT(r.error.find("amber_missing_rel88.txt") != std::string::npos);
}

// The lower bounds clamp as well as the upper: offset/limit below 1 behave as
// 1, so exactly the first line comes back.
TEST(read_tool_clamps_offset_and_limit_floor) {
    agent::Workspace::set_root("/tmp");
    const std::string path = "/tmp/amber_read_clamp.txt";
    {
        std::ofstream f(path);
        for (int i = 1; i <= 5; ++i)
            f << "line " << i << "\n";
    }
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", path}, {"offset", 0}, {"limit", 0}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("1:\tline 1") != std::string::npos);
    ASSERT(r.output.find("2:\tline 2") == std::string::npos);
    ASSERT_EQ(r.meta["lines"].get<long>(), 1L);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// write tool (patch style)
// ---------------------------------------------------------------------------

TEST(write_tool_create_then_patch) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();

    auto r = tool->execute({{"path", path}, {"edits", {{{"old", ""}, {"new", "alpha\nbeta\n"}}}}});
    ASSERT_TRUE(r.ok);
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\nbeta\n");
    }

    auto r2 = tool->execute({{"path", path}, {"edits", {{{"old", "beta"}, {"new", "gamma"}}}}});
    ASSERT_TRUE(r2.ok);
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\ngamma\n");
    }
    std::remove(path.c_str());
}

TEST(write_tool_missing_old_fails) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test2.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();
    auto r = tool->execute({{"path", path}, {"edits", {{{"old", "nope"}, {"new", "x"}}}}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("not found") != std::string::npos);
    std::remove(path.c_str());
}

TEST(write_tool_result_reports_relative_path) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_rel88.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();
    auto r = tool->execute({{"path", path}, {"edits", {{{"old", ""}, {"new", "x\n"}}}}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("/tmp/amber_write_rel88.txt") == std::string::npos);
    ASSERT(r.output.find("amber_write_rel88.txt") != std::string::npos);
    ASSERT_EQ(r.meta.value("path", ""), "amber_write_rel88.txt");
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// workspace path confinement
// ---------------------------------------------------------------------------

TEST(workspace_confines_relative_and_rejects_escape) {
    agent::Workspace::set_root("/tmp/amber_ws");
    std::string resolved, err;

    ASSERT_TRUE(agent::Workspace::confine("a/b.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/a/b.txt");

    ASSERT_TRUE(agent::Workspace::confine("./x/../y.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/y.txt");

    ASSERT_FALSE(agent::Workspace::confine("../../etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    ASSERT_FALSE(agent::Workspace::confine("/etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    // sibling directory sharing a prefix must not be treated as inside
    ASSERT_FALSE(agent::Workspace::confine("/tmp/amber_ws2/x", resolved, err));
}

// A window's worktree is a per-RUN override: tools operate inside it, while
// state, config and policy stay keyed to the project root every window shares.
TEST(workspace_run_scope_overrides_the_tool_root) {
    agent::Workspace::set_root("/tmp/amber_scope_project");
    ASSERT_EQ(agent::Workspace::workspace_root(), "/tmp/amber_scope_project");
    ASSERT_EQ(agent::Workspace::project_root(), "/tmp/amber_scope_project");

    const std::string worktree = "/tmp/amber_scope_worktree";
    agent::RunScope scope;
    scope.workspace_root = &worktree;
    agent::ScopedRunScope guard(scope);

    ASSERT_EQ(agent::Workspace::workspace_root(), worktree);
    ASSERT_EQ(agent::Workspace::project_root(), "/tmp/amber_scope_project");

    std::string resolved, err;
    ASSERT_TRUE(agent::Workspace::confine("src/main.cpp", resolved, err));
    ASSERT_EQ(resolved, worktree + "/src/main.cpp");

    // The project root is not the tool root while a worktree is active.
    ASSERT_FALSE(agent::Workspace::confine("/tmp/amber_scope_project/x", resolved, err));
    ASSERT_EQ(agent::Workspace::relative(worktree + "/lib/a.cpp"), "lib/a.cpp");
}

// The chain a dispatch worker re-installs must carry the root even when no
// cancel token or catalog rides along with it.
TEST(workspace_run_scope_survives_the_chain_hop) {
    agent::Workspace::set_root("/tmp/amber_chain_project");
    const std::string worktree = "/tmp/amber_chain_worktree";

    agent::RunScope scope;
    scope.workspace_root = &worktree;
    agent::RunScopeChain captured;
    {
        agent::ScopedRunScope guard(scope);
        captured = agent::capture_run_scope();
    }
    ASSERT_EQ(agent::Workspace::workspace_root(), "/tmp/amber_chain_project");

    agent::ScopedRunScope hop(captured);
    ASSERT_EQ(agent::Workspace::workspace_root(), worktree);
    ASSERT_EQ(agent::Workspace::project_root(), "/tmp/amber_chain_project");
}

// ---------------------------------------------------------------------------
// project state layout (XDG)
// ---------------------------------------------------------------------------

// The slug is what keeps two projects' state apart, so it must be stable,
// readable, and collision-free even for paths that sanitize to the same name.

TEST(workspace_scoped_project_root_drives_state_and_tools) {
    agent::Workspace::set_root("/tmp/amber_project_default");
    const std::string scoped = "/tmp/amber_project_other";
    {
        agent::ScopedProjectRoot guard(scoped);
        ASSERT_EQ(agent::Workspace::project_root(), scoped);
        ASSERT_EQ(agent::Workspace::workspace_root(), scoped);
        ASSERT_EQ(agent::Workspace::state_dir(), agent::Workspace::state_dir_for(scoped));
        ASSERT_EQ(agent::Workspace::settings_path(), agent::Workspace::settings_path_for(scoped));
        ASSERT_EQ(agent::Workspace::local_dir(), scoped + "/.amber");

        std::string run_project = "/tmp/amber_project_run";
        agent::RunScope scope;
        scope.project_root = &run_project;
        agent::ScopedRunScope run_guard(scope);
        ASSERT_EQ(agent::Workspace::project_root(), run_project);
        ASSERT_EQ(agent::Workspace::workspace_root(), run_project);
    }
    ASSERT_EQ(agent::Workspace::project_root(), "/tmp/amber_project_default");
    ASSERT_EQ(agent::Workspace::process_project_root(), "/tmp/amber_project_default");
    ASSERT_EQ(agent::Workspace::state_dir(),
              agent::Workspace::state_dir_for("/tmp/amber_project_default"));
}

TEST(workspace_project_settings_and_policy_adopt_local_files) {
    namespace fs = std::filesystem;
    const std::string root = "/tmp/amber_project_settings";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/.amber", ec);
    {
        std::ofstream f(root + "/.amber/settings", std::ios::trunc);
        f << "compression_threshold=0.5\n";
    }
    {
        std::ofstream f(root + "/.amber/policy.json", std::ios::trunc);
        f << "{}\n";
    }
    const std::string settings = agent::Workspace::settings_path_for(root);
    const std::string policy = agent::Workspace::policy_path_for(root);
    ASSERT_TRUE(fs::exists(settings));
    ASSERT_TRUE(fs::exists(policy));
    ASSERT_EQ(settings, agent::Workspace::state_dir_for(root) + "/settings");
    ASSERT_EQ(policy, agent::Workspace::state_dir_for(root) + "/policy.json");
    ASSERT_EQ(agent::Workspace::local_dir_for(root), root + "/.amber");
    fs::remove_all(root, ec);
}

TEST(workspace_state_slug_is_stable_and_collision_free) {
    const std::string a = agent::workspace_state_slug("/home/jack/Projects/amber");
    ASSERT_EQ(a, agent::workspace_state_slug("/home/jack/Projects/amber"));
    ASSERT(a.find("home-jack-Projects-amber") == 0);

    // Sanitizing alone would map both of these onto "home-u-work-foo-bar";
    // the hash suffix is what stops them from sharing sessions.
    ASSERT(agent::workspace_state_slug("/home/u/work/foo-bar") !=
           agent::workspace_state_slug("/home/u/work/foo_bar"));

    // A very deep path is bounded rather than overflowing a filename.
    const std::string deep = "/" + std::string(300, 'a');
    ASSERT(agent::workspace_state_slug(deep).size() < 80u);
}

// State lives under the XDG state root, keyed by the workspace -- never in the
// project tree, so running amber in $HOME leaves no ~/.amber behind.
TEST(workspace_state_dir_is_per_project_and_outside_the_tree) {
    const std::string saved_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* saved_xdg = std::getenv("XDG_STATE_HOME");
    const std::string saved_xdg_state = saved_xdg ? saved_xdg : "";
    setenv("HOME", "/tmp/amber_state_home", 1);
    unsetenv("XDG_STATE_HOME");

    agent::Workspace::set_root("/tmp/amber_state_home");
    const std::string dir = agent::Workspace::state_dir();
    ASSERT_EQ(dir, "/tmp/amber_state_home/.local/state/amber/projects/" +
                       agent::workspace_state_slug("/tmp/amber_state_home"));
    // The whole point of the move: home-as-workspace no longer implies
    // <home>/.amber as the state dir.
    ASSERT(dir.find("/tmp/amber_state_home/.amber") == std::string::npos);

    // An XDG_STATE_HOME override wins, and a hostile one is ignored.
    setenv("XDG_STATE_HOME", "/tmp/amber_state_override", 1);
    ASSERT(agent::Workspace::state_dir().find("/tmp/amber_state_override/amber/projects/") == 0);
    setenv("XDG_STATE_HOME", "relative/path", 1);
    ASSERT(agent::Workspace::state_dir().find("/tmp/amber_state_home/.local/state/") == 0);
    setenv("XDG_STATE_HOME", "/tmp/../etc", 1);
    ASSERT(agent::Workspace::state_dir().find("/tmp/amber_state_home/.local/state/") == 0);
    unsetenv("XDG_STATE_HOME");

    // state_dir() is a pure path: asking for it must not create it.
    run_cmd("rm -rf /tmp/amber_state_home");
    agent::Workspace::set_root("/tmp/amber_state_home");
    ASSERT_EQ(agent::Workspace::state_dir(), dir);
    ASSERT_FALSE(std::filesystem::exists("/tmp/amber_state_home"));

    if (!saved_home.empty())
        setenv("HOME", saved_home.c_str(), 1);
    if (!saved_xdg_state.empty())
        setenv("XDG_STATE_HOME", saved_xdg_state.c_str(), 1);
}

// The one-time adoption: an artefact that predates the move is copied into the
// state dir, and the original is left where it was.
TEST(workspace_adopts_legacy_state_without_deleting_it) {
    const std::string saved_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* saved_xdg = std::getenv("XDG_STATE_HOME");
    const std::string saved_xdg_state = saved_xdg ? saved_xdg : "";
    setenv("HOME", "/tmp/amber_state_adopt_home", 1);
    unsetenv("XDG_STATE_HOME");

    run_cmd("rm -rf /tmp/amber_state_adopt /tmp/amber_state_adopt_home");
    run_cmd("mkdir -p /tmp/amber_state_adopt/.amber/sessions");
    {
        std::ofstream f("/tmp/amber_state_adopt/.amber/sessions/keep.json");
        f << "{\"id\":\"keep\"}\n";
    }
    {
        std::ofstream f("/tmp/amber_state_adopt/.amber/settings");
        f << "model=legacy-model\n";
    }
    agent::Workspace::set_root("/tmp/amber_state_adopt");

    agent::Workspace::adopt_legacy_state("sessions");
    agent::Workspace::adopt_legacy_state("settings");
    ASSERT(std::filesystem::exists(agent::Workspace::state_dir() + "/sessions/keep.json"));
    ASSERT(std::filesystem::exists(agent::Workspace::state_dir() + "/settings"));
    // The originals stay: a layout change must not delete data.
    ASSERT(std::filesystem::exists("/tmp/amber_state_adopt/.amber/sessions/keep.json"));
    ASSERT(std::filesystem::exists("/tmp/amber_state_adopt/.amber/settings"));

    // A second adoption must not clobber what the state dir already holds.
    {
        std::ofstream f(agent::Workspace::state_dir() + "/settings");
        f << "model=newer-model\n";
    }
    agent::Workspace::adopt_legacy_state("settings");
    std::ifstream f(agent::Workspace::state_dir() + "/settings");
    std::stringstream ss;
    ss << f.rdbuf();
    ASSERT(ss.str().find("newer-model") != std::string::npos);

    // The artefact names the consumers actually use, through the accessors
    // they call: a typo here would be a user silently losing their remembered
    // approvals, so it is pinned rather than assumed.
    {
        std::ofstream pf("/tmp/amber_state_adopt/.amber/policy.json");
        pf << "{\"rules\":[]}\n";
    }
    ASSERT_EQ(agent::Workspace::settings_path(), agent::Workspace::state_dir() + "/settings");
    ASSERT_EQ(agent::Workspace::policy_path(), agent::Workspace::state_dir() + "/policy.json");
    ASSERT(std::filesystem::exists(agent::Workspace::state_dir() + "/policy.json"));

    if (!saved_home.empty())
        setenv("HOME", saved_home.c_str(), 1);
    if (!saved_xdg_state.empty())
        setenv("XDG_STATE_HOME", saved_xdg_state.c_str(), 1);
}

// Regenerable data belongs in the cache root, not the config tree.
TEST(cache_dir_is_the_xdg_cache_root) {
    const char* saved = std::getenv("XDG_CACHE_HOME");
    const std::string saved_cache = saved ? saved : "";
    setenv("XDG_CACHE_HOME", "/tmp/amber_cache_override", 1);
    ASSERT_EQ(agent::cache_dir(), "/tmp/amber_cache_override/amber");
    setenv("XDG_CACHE_HOME", "relative/path", 1);
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    ASSERT_EQ(agent::cache_dir(), home + "/.cache/amber");
    if (!saved_cache.empty())
        setenv("XDG_CACHE_HOME", saved_cache.c_str(), 1);
    else
        unsetenv("XDG_CACHE_HOME");
}

TEST(read_write_tools_reject_paths_outside_workspace) {
    agent::Workspace::set_root("/tmp/amber_ws_tools");
    run_cmd("mkdir -p /tmp/amber_ws_tools");

    auto rtool = agent::make_read_tool();
    auto rr = rtool->execute({{"path", "/etc/passwd"}});
    ASSERT_FALSE(rr.ok);
    ASSERT(rr.error.find("workspace") != std::string::npos);

    auto wtool = agent::make_write_tool();
    auto wr = wtool->execute({{"path", "../escape.txt"}, {"edits", {{{"old", ""}, {"new", "x"}}}}});
    ASSERT_FALSE(wr.ok);
    ASSERT(wr.error.find("workspace") != std::string::npos);
}

TEST(workspace_rejects_symlink_escape) {
    std::string root = "/tmp/amber_ws_symlink";
    std::string outside = "/tmp/amber_outside_symlink";
    run_cmd("rm -rf " + root + " " + outside + " && mkdir -p " + root + " && mkdir -p " + outside);
    agent::Workspace::set_root(root);

    // A symlink inside the workspace that points outside must not be readable
    // or writable through the confined tools.
    run_cmd("ln -s " + outside + " " + root + "/escape");
    {
        std::ofstream secret(outside + "/secret.txt");
        secret << "top secret\n";
    }
    auto rtool = agent::make_read_tool();
    auto rr = rtool->execute({{"path", "escape/secret.txt"}});
    ASSERT_FALSE(rr.ok);

    auto wtool = agent::make_write_tool();
    auto wr =
        wtool->execute({{"path", "escape/pwned.txt"}, {"edits", {{{"old", ""}, {"new", "x"}}}}});
    ASSERT_FALSE(wr.ok);
    ASSERT_FALSE(std::filesystem::exists(outside + "/pwned.txt"));

    // A symlink that stays inside the workspace remains usable: `inner` ->
    // root, so inner/real/file.txt is root/real/file.txt.
    run_cmd("ln -s " + root + " " + root + "/inner && mkdir -p " + root + "/real");
    std::string resolved, err;
    ASSERT_TRUE(agent::Workspace::confine("inner/real/file.txt", resolved, err));
    ASSERT_EQ(resolved, root + "/inner/real/file.txt");
    auto rtool2 = agent::make_read_tool();
    auto rr2 = rtool2->execute({{"path", "inner/real/file.txt"}});
    ASSERT_FALSE(rr2.ok); // file does not exist yet — but confine must not reject the path
}

// ---------------------------------------------------------------------------
// search backends
// ---------------------------------------------------------------------------

namespace {
std::string make_search_tree() {
    std::string dir = "/tmp/amber_srch";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir + "/sub";
    run_cmd(cmd);
    {
        std::ofstream f(dir + "/a.cpp");
        f << "void register_default_tools() {}\nint helper() { return 1; }\n";
    }
    {
        std::ofstream f(dir + "/sub/b.cpp");
        f << "void register_default_tools() {}\n// unrelated content\n";
    }
    {
        std::ofstream f(dir + "/note.txt");
        f << "register_default_tools is the function we want to find\n";
    }
    return dir;
}
} // namespace

TEST(search_grep_backend) {
    std::string dir = make_search_tree();
    auto be = agent::make_grep_backend();
    auto hits = be->search("register_default_tools", dir, "*.cpp", 100);
    ASSERT_EQ(be->name(), "grep");
    ASSERT_FALSE(hits.empty());
    bool saw_a = false, saw_b = false;
    for (const auto& h : hits) {
        if (h.path.find("a.cpp") != std::string::npos)
            saw_a = true;
        if (h.path.find("b.cpp") != std::string::npos)
            saw_b = true;
        ASSERT(h.line_no > 0);
    }
    ASSERT(saw_a && saw_b);
    run_cmd("rm -rf " + dir);
}

TEST(search_grep_backend_resists_shell_injection) {
    std::string dir = make_search_tree();
    std::string sentinel = "/tmp/amber_pwned";
    run_cmd("rm -f " + sentinel);
    auto be = agent::make_grep_backend();
    // A query crafted to break out of the command if quoting were absent.
    auto hits = be->search("x'; touch " + sentinel + "; echo '", dir, "", 100);
    // The injected command must not have run.
    ASSERT_FALSE(access(sentinel.c_str(), F_OK) == 0);
    (void)hits;
    run_cmd("rm -rf " + dir + " " + sentinel);
}

TEST(search_semantic_backend_ranks_relevant) {
    std::string dir = make_search_tree();
    auto be = agent::make_semantic_backend();
    auto hits = be->search("register the default tools function", dir, "", 5);
    ASSERT_EQ(be->name(), "semantic");
    ASSERT_FALSE(hits.empty());
    // The line containing register_default_tools should rank at or near the top.
    bool top_has_target = hits[0].line.find("register_default_tools") != std::string::npos;
    ASSERT(top_has_target);
    ASSERT(hits[0].score > 0.0);
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// SEC-03: search backend hardening (Red tests).
// GrepBackend must reject option-injection patterns (those starting with -
// must be treated as patterns, not grep flags). SemanticIndex must not
// follow symlinks that escape the workspace root. SearchTool must clamp
// max to a sane upper bound.
// ---------------------------------------------------------------------------

TEST(sec03_grep_backend_pattern_starting_with_dash) {
    // A pattern starting with "-" must be treated as a pattern, not a
    // grep option. Without "--" before the pattern, grep would interpret
    // "-foo" as an unknown option and fail (or worse, match a different
    // flag). The backend must still find the literal text.
    std::string dir = make_search_tree();
    // Add a file containing a dash-prefixed token.
    {
        std::ofstream f(dir + "/dash.cpp");
        f << "int -foo bar;\n";
    }
    auto be = agent::make_grep_backend();
    auto hits = be->search("-foo", dir, "*.cpp", 100);
    ASSERT_FALSE(hits.empty());
    bool found = false;
    for (const auto& h : hits) {
        if (h.line.find("-foo") != std::string::npos)
            found = true;
    }
    ASSERT(found);
    run_cmd("rm -rf " + dir);
}

TEST(sec03_semantic_index_skips_symlink_escape) {
    // A symlink inside the workspace pointing to a file outside must not
    // be indexed or returned by the semantic backend.
    std::string dir = "/tmp/amber_sec03_sym";
    std::string outside = "/tmp/amber_sec03_outside";
    run_cmd("rm -rf " + dir + " " + outside);
    std::filesystem::create_directories(dir);
    std::filesystem::create_directories(outside);
    // Outside file with a unique marker.
    {
        std::ofstream f(outside + "/secret.cpp");
        f << "int SEC03_SYMLINK_ESCAPE_MARKER = 1;\n";
    }
    // Inside file with normal content.
    {
        std::ofstream f(dir + "/inside.cpp");
        f << "int normal_code = 0;\n";
    }
    // Symlink inside the workspace pointing to the outside file.
    std::filesystem::create_symlink(outside + "/secret.cpp", dir + "/link.cpp");

    auto be = agent::make_semantic_backend();
    auto hits = be->search("SEC03_SYMLINK_ESCAPE_MARKER", dir, "*.cpp", 100);
    // The symlinked file must NOT appear in results: no hit path should
    // contain "link.cpp" (the symlink).
    for (const auto& h : hits) {
        ASSERT(h.path.find("link.cpp") == std::string::npos);
    }
    run_cmd("rm -rf " + dir + " " + outside);
}

TEST(sec03_search_tool_clamps_max) {
    // SearchTool must clamp max to a reasonable upper bound so a model
    // cannot request an unbounded result set.
    agent::Workspace::set_root("/tmp");
    auto tool = agent::make_search_tool(agent::builtin_search_backend_provider());
    // A huge max must not crash or produce an unbounded query.
    auto r = tool->execute({{"pattern", "x"}, {"max", 999999999}});
    ASSERT_TRUE(r.ok); // it runs, just clamped
    run_cmd("rm -rf /tmp/amber_sec03_sym /tmp/amber_sec03_outside");
}

TEST(search_tool_mode_switch) {
    std::string dir = make_search_tree();
    agent::Workspace::set_root(dir);
    auto tool = agent::make_search_tool(agent::builtin_search_backend_provider());

    auto g = tool->execute({{"pattern", "register_default_tools"},
                            {"path", dir},
                            {"glob", "*.cpp"},
                            {"mode", "grep"}});
    ASSERT_TRUE(g.ok);
    ASSERT(g.output.find("[grep]") != std::string::npos);

    auto s = tool->execute(
        {{"pattern", "register the default tools"}, {"path", dir}, {"mode", "semantic"}});
    ASSERT_TRUE(s.ok);
    ASSERT(s.output.find("[semantic]") != std::string::npos);
    run_cmd("rm -rf " + dir);
    agent::Workspace::set_root("."); // leave a valid root for later tests
}

// ---------------------------------------------------------------------------
// Server auto-detection: /v1/models parsing (pure, no network)
// ---------------------------------------------------------------------------

TEST(probe_parse_llamacpp_models) {
    // Real llama.cpp /v1/models shape (trimmed): id + meta.n_ctx/n_ctx_train.
    std::string body = R"({"object":"list","data":[{"id":"Qwopus3.6-27B.gguf",)"
                       R"("object":"model","owned_by":"llamacpp","meta":{"n_vocab":248320,)"
                       R"("n_ctx":262144,"n_ctx_train":262144,"n_embd":5120}}]})";
    agent::ServerInfo info = agent::make_dialect("openai")->parse_models_response(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "Qwopus3.6-27B.gguf");
    ASSERT_EQ(info.context_size, 262144);
    ASSERT_EQ(info.context_train, 262144);
}

TEST(probe_parse_models_array_fallback) {
    // Ollama-ish {"models":[{"name":..,"n_ctx":..}]} fallback shape.
    std::string body = R"({"models":[{"name":"llama-3.2-3b","n_ctx":8192}]})";
    agent::ServerInfo info = agent::make_dialect("openai")->parse_models_response(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "llama-3.2-3b");
    ASSERT_EQ(info.context_size, 8192);
}

TEST(probe_parse_models_malformed_is_not_ok) {
    ASSERT_FALSE(agent::make_dialect("openai")->parse_models_response("not json").ok);
    ASSERT_FALSE(agent::make_dialect("openai")->parse_models_response("{}").ok);
    ASSERT_FALSE(agent::make_dialect("openai")->parse_models_response(R"({"data":[]})").ok);
}

// OpenAI-compatible gateways (kilocode, OpenRouter, ...) advertise the window
// as a top-level context_length with meta absent. Regression: the probe only
// read n_ctx/meta.n_ctx, so kilo-auto/free (context_length: 256000) yielded an
// unknown window — the TUI gauge stayed hidden and auto-compression was
// disabled.
TEST(probe_parse_kilocode_context_length) {
    std::string body = R"({"data":[{"id":"kilo-auto/free","object":"model",)"
                       R"("owned_by":"kilo","context_length":256000,"meta":null},)"
                       R"({"id":"kilo-auto/frontier","object":"model",)"
                       R"("owned_by":"kilo","context_length":1000000,"meta":null}]})";
    agent::ServerInfo info = agent::make_dialect("openai")->parse_models_response(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "kilo-auto/free");
    ASSERT_EQ(info.context_size, 256000);
    ASSERT_EQ(info.context_train, 256000);
}

// When a server reports both n_ctx (llama.cpp) and context_length, the
// llama.cpp shape wins — it is the loaded model's real window.
TEST(probe_prefers_n_ctx_over_context_length) {
    std::string body = R"({"data":[{"id":"hybrid","object":"model",)"
                       R"("meta":{"n_ctx":32768,"n_ctx_train":32768},"context_length":131072}]})";
    agent::ServerInfo info = agent::make_dialect("openai")->parse_models_response(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.context_size, 32768);
}

// The window must come from the ACTIVE model, not from the first entry in
// the server's list (a router may list models without context metadata
// ahead of the one the user actually runs).
TEST(probe_prefers_the_active_model) {
    std::string body = R"({"data":[{"id":"Devstral-Small-2-24B","object":"model",)"
                       R"("owned_by":"llamacpp"},{"id":"Qwopus3.6-27B-Fusion","object":"model",)"
                       R"("owned_by":"llamacpp","meta":{"n_ctx":262144,"n_ctx_train":262144}}]})";
    agent::ServerInfo info =
        agent::make_dialect("openai")->parse_models_response(body, "Qwopus3.6-27B-Fusion");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "Qwopus3.6-27B-Fusion");
    ASSERT_EQ(info.context_size, 262144);
}

TEST(probe_active_model_without_meta_is_unknown) {
    std::string body = R"({"data":[{"id":"Devstral-Small-2-24B","object":"model",)"
                       R"("owned_by":"llamacpp"},{"id":"Qwopus3.6-27B-Fusion","object":"model",)"
                       R"("owned_by":"llamacpp","meta":{"n_ctx":262144,"n_ctx_train":262144}}]})";
    agent::ServerInfo info =
        agent::make_dialect("openai")->parse_models_response(body, "Devstral-Small-2-24B");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "Devstral-Small-2-24B");
    ASSERT_EQ(info.context_size, 0); // honest: no metadata, no fabrication
}

TEST(probe_falls_back_to_first_model_with_context) {
    std::string body = R"({"data":[{"id":"Devstral-Small-2-24B","object":"model",)"
                       R"("owned_by":"llamacpp"},{"id":"Qwopus3.6-27B-Fusion","object":"model",)"
                       R"("owned_by":"llamacpp","meta":{"n_ctx":262144,"n_ctx_train":262144}}]})";
    agent::ServerInfo info = agent::make_dialect("openai")->parse_models_response(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.context_size, 262144);
}

// The model LIST parser keeps per-model context info so the /set model drawer
// can show "id (ctx N)" inline instead of bare ids.
TEST(probe_parse_model_list_with_ctx) {
    std::string body = R"({"data":[{"id":"alpha","meta":{"n_ctx":8192,"n_ctx_train":32768}},)"
                       R"({"id":"beta"}]})";
    auto models = agent::make_dialect("openai")->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 2u);
    ASSERT_EQ(models[0].id, "alpha");
    ASSERT_EQ(models[0].context, 8192);
    ASSERT_EQ(models[0].context_train, 32768);
    ASSERT_EQ(models[1].id, "beta");
    ASSERT_EQ(models[1].context, 0);
    ASSERT_EQ(models[1].context_train, 0);
}

TEST(probe_parse_model_list_malformed) {
    ASSERT(agent::make_dialect("openai")->parse_model_list_response("not json").empty());
    ASSERT(agent::make_dialect("openai")->parse_model_list_response("{}").empty());
    ASSERT(agent::make_dialect("openai")->parse_model_list_response(R"({"data":[]})").empty());
}

// A gateway that lists `anthropic/claude-*` often reports only the id. The
// documented family fills limits and the thinking contract, so the picker and
// the request are not left blank; a non-Claude id stays unknown.
TEST(probe_parse_model_list_fills_documented_claude_families) {
    const std::string body = R"({"data":[)"
                             R"({"id":"anthropic/claude-sonnet-5"},)"
                             R"({"id":"anthropic/claude-opus-4-5"},)"
                             R"({"id":"anthropic/claude-haiku-4-5","context_length":200000},)"
                             R"({"id":"openai/gpt-4o"}]})";
    auto models = agent::make_dialect("openai")->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 4u);

    ASSERT_EQ(models[0].context, 1000000);
    ASSERT_EQ(models[0].max_output, 128000);
    ASSERT_TRUE(models[0].reasoning);
    ASSERT_TRUE(models[0].thinking_adaptive);
    ASSERT_EQ(models[0].efforts.size(), 5u);
    ASSERT_EQ(models[0].default_effort, std::string("high"));

    ASSERT_EQ(models[1].context, 200000);
    ASSERT_EQ(models[1].max_output, 64000);
    ASSERT_TRUE(models[1].thinking_legacy);
    ASSERT_FALSE(models[1].thinking_adaptive);
    ASSERT_EQ(models[1].default_effort, std::string("high"));

    // A reported limit still wins over the family fallback.
    ASSERT_EQ(models[2].context, 200000);
    ASSERT_EQ(models[2].max_output, 64000);

    // The table is bounded to Claude ids: no invented limits elsewhere.
    ASSERT_EQ(models[3].context, 0);
    ASSERT_EQ(models[3].max_output, 0);
    ASSERT_FALSE(models[3].reasoning);
}

// Claude 5.x/4.6+ reject a non-default sampling temperature with a 400. The
// OpenAI-compatible dialect reaches them through gateways, so it applies the
// same rule as the Messages dialect; every other model keeps amber's value.
TEST(openai_request_omits_temperature_only_for_adaptive_claude) {
    auto d = agent::make_dialect("openai");
    agent::Config cfg;
    cfg.temperature = 0.2;
    cfg.model = "anthropic/claude-sonnet-5";
    auto body = d->build_chat_body(cfg, {}, {}, false);
    ASSERT(!body.contains("temperature"));

    cfg.model = "anthropic/claude-sonnet-4-5";
    body = d->build_chat_body(cfg, {}, {}, false);
    ASSERT(body.contains("temperature"));
    ASSERT_EQ(body["temperature"].get<double>(), 0.2);

    cfg.model = "gpt-4o";
    body = d->build_chat_body(cfg, {}, {}, false);
    ASSERT(body.contains("temperature"));
}

TEST(probe_parse_model_list_ollama_shape) {
    // Ollama-ish {"models":[{name, n_ctx}]} fallback shape, two entries.
    std::string body = R"({"models":[{"name":"llama-3.2-3b","n_ctx":8192},)"
                       R"({"name":"qwen-7b"}]})";
    auto models = agent::make_dialect("openai")->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 2u);
    ASSERT_EQ(models[0].id, "llama-3.2-3b");
    ASSERT_EQ(models[0].context, 8192);
    ASSERT_EQ(models[1].id, "qwen-7b");
}

// llama.cpp "automatic parser generation" failures are a server/model problem
// (the loaded chat template cannot be auto-parsed for tool calling). The error
// message must say so instead of dumping the raw body.
TEST(http_error_describes_parser_generation_failure) {
    std::string body = R"({"error":{"code":400,"message":"Unable to generate parser for this )"
                       R"(template. Automatic parser generation failed: [json.exception.)"
                       R"(type_error.302] type must be array, but is null","type":")"
                       R"(invalid_request_error"}})";
    std::string msg = agent::describe_http_error(400, body);
    ASSERT(msg.find("chat-template parser failure") != std::string::npos);
    ASSERT(msg.find("reload the model") != std::string::npos);
    // Unrelated 400s keep the plain message (no misleading hint).
    std::string plain = agent::describe_http_error(400, R"({"error":"nope"})");
    ASSERT(plain.find("chat-template") == std::string::npos);
    ASSERT(plain.find("HTTP 400 from LLM server") != std::string::npos);
}

// A gateway (kilocode) that loses its upstream mid-request returns HTTP 400
// whose body is an empty SSE stream ("data: \n[DONE]") — a transient blip that
// must retry, not a request rejection. Regression: the retry classifier only
// retried 429/5xx, so one upstream hiccup aborted the whole turn.
TEST(http_error_empty_stream_400_is_retryable) {
    // The exact kilocode shape from a live failure.
    ASSERT_TRUE(agent::make_dialect("openai")->is_retryable(400, "data: \n[DONE]\n\n"));
    // Keep-alive comments plus a bare [DONE] are also empty.
    ASSERT_TRUE(agent::make_dialect("openai")->is_retryable(
        400, ": KILO PROCESSING\n: KILO PROCESSING\ndata: [DONE]\n\n"));
    // Plain empty body.
    ASSERT_TRUE(agent::make_dialect("openai")->is_retryable(400, ""));
    // 429 and 5xx stay retryable regardless of body.
    ASSERT_TRUE(agent::make_dialect("openai")->is_retryable(429, R"({"error":"rate"})"));
    ASSERT_TRUE(agent::make_dialect("openai")->is_retryable(502, "error code: 1101"));
}

TEST(http_error_json_400_is_not_retryable) {
    // Genuine request rejections (schema/model/auth) carry a JSON error body.
    ASSERT_FALSE(agent::make_dialect("openai")->is_retryable(
        400, R"({"error":{"message":"Bad request","type":"invalid_request_error"}})"));
    // A data payload in the stream means the upstream responded — real body.
    ASSERT_FALSE(agent::make_dialect("openai")->is_retryable(
        400, "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n"));
    // Non-400, non-retryable stays put.
    ASSERT_FALSE(agent::make_dialect("openai")->is_retryable(401, R"({"error":"auth"})"));
    ASSERT_FALSE(agent::make_dialect("openai")->is_retryable(403, R"({"error":"forbidden"})"));
}

// The auto-detect merge policy: probe results fill only fields the user left on
// auto; explicit values are never overwritten. Network-free (merge_server_info).
TEST(autodetect_fills_only_auto_fields) {
    agent::ServerInfo info;
    info.ok = true;
    info.model = "server-model";
    info.context_size = 262144;

    // Both auto -> both filled.
    agent::Config a; // defaults: model_explicit=false, context_explicit=false
    agent::merge_server_info(a, info);
    ASSERT_EQ(a.model, "server-model");
    ASSERT_EQ(a.context_size, 262144);

    // Both explicit -> untouched.
    agent::Config b;
    b.model = "user-model";
    b.model_explicit = true;
    b.context_size = 4096;
    b.context_explicit = true;
    agent::merge_server_info(b, info);
    ASSERT_EQ(b.model, "user-model");
    ASSERT_EQ(b.context_size, 4096);

    // Mixed -> only the auto one changes.
    agent::Config c;
    c.model = "user-model";
    c.model_explicit = true; // pinned
    c.context_size = 0;
    c.context_explicit = false; // auto
    agent::merge_server_info(c, info);
    ASSERT_EQ(c.model, "user-model");
    ASSERT_EQ(c.context_size, 262144);
}

// An unreachable / not-ok probe must never mutate the config.
TEST(autodetect_noop_when_server_down) {
    agent::ServerInfo down; // ok defaults to false
    down.model = "ghost";
    down.context_size = 999;
    agent::Config c;
    c.model = "keep"; // auto, but server is down
    agent::merge_server_info(c, down);
    ASSERT_EQ(c.model, "keep");
    ASSERT_EQ(c.context_size, 0);
}

// ---------------------------------------------------------------------------
// Status-bar rendering math (pure, no ncurses / no network)
// ---------------------------------------------------------------------------

TEST(compact_count_never_grows_the_field) {
    using agent::compact::count;
    ASSERT_EQ(count(-1), "?");
    ASSERT_EQ(count(0), "0");
    ASSERT_EQ(count(999), "999");
    ASSERT_EQ(count(1000), "1k");
    ASSERT_EQ(count(1500), "1.5k");
    ASSERT_EQ(count(5000), "5k");
    ASSERT_EQ(count(12500), "13k");
    ASSERT_EQ(count(128000), "130k");
    ASSERT_EQ(count(995000), "1m"); // 999k would be four characters
    ASSERT_EQ(count(1000000), "1m");
    ASSERT_EQ(count(1500000), "1.5m");
    ASSERT_EQ(count(12300000), "12m");
}

TEST(compact_duration_uses_the_largest_sensible_unit) {
    using agent::compact::duration_ms;
    ASSERT_EQ(duration_ms(-1), "?");
    ASSERT_EQ(duration_ms(240), "240ms");
    ASSERT_EQ(duration_ms(999), "999ms");
    ASSERT_EQ(duration_ms(1000), "1s");
    ASSERT_EQ(duration_ms(1500), "1.5s");
    ASSERT_EQ(duration_ms(12345), "12s");
    ASSERT_EQ(duration_ms(59900), "60s");
    ASSERT_EQ(duration_ms(90000), "1.5m");
    ASSERT_EQ(duration_ms(3540000), "59m");
    ASSERT_EQ(duration_ms(3600000), "1h");
    ASSERT_EQ(duration_ms(5400000), "1.5h");
}

TEST(compact_percent_is_bounded) {
    using agent::compact::percent;
    ASSERT_EQ(percent(-5), "0");
    ASSERT_EQ(percent(0), "0");
    ASSERT_EQ(percent(42), "42");
    ASSERT_EQ(percent(100), "100");
    ASSERT_EQ(percent(101), "100");
}

TEST(compact_bytes_uses_binary_units) {
    using agent::compact::bytes;
    ASSERT_EQ(bytes(-1), "?");
    ASSERT_EQ(bytes(512), "512B");
    ASSERT_EQ(bytes(1024), "1KB");
    ASSERT_EQ(bytes(1536), "1.5KB");
    ASSERT_EQ(bytes(12800), "13KB");
    ASSERT_EQ(bytes(1048576), "1MB");
    ASSERT_EQ(bytes(3145728), "3MB");
    ASSERT_EQ(bytes(1073741824L), "1GB");
}

TEST(usage_totals_sum_reported_and_ignore_unknown) {
    agent::UsageTotals u;
    ASSERT_FALSE(u.measured);
    u.add(-1, -1); // a provider that reports no usage is unknown, not zero
    ASSERT_FALSE(u.measured);
    ASSERT_EQ(u.prompt_tokens, 0L);
    u.add(100, 20);
    u.add(-1, 5);
    ASSERT_EQ(u.prompt_tokens, 100L);
    ASSERT_EQ(u.completion_tokens, 25L);
    ASSERT_TRUE(u.measured);
}

TEST(statusbar_pressure_thresholds) {
    ASSERT(agent::bar::pressure(0.0) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.59) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.60) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.85) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.851) == agent::bar::Pressure::Crit);
    ASSERT(agent::bar::pressure(1.0) == agent::bar::Pressure::Crit);
}

TEST(statusbar_gauge_fill_cells) {
    // Empty and full extremes.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.0, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(1.0, 10), 10);
    // Half fill of 10 cells = 5 full cells.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 10), 5);
    // Clamps out-of-range fractions.
    ASSERT_EQ(agent::bar::gauge_full_cells(-0.5, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(2.0, 10), 10);
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 0), 0);
}

TEST(statusbar_gauge_bar_glyphs) {
    // Empty bar is all light-shade track (\u2591), one per cell (3 bytes each).
    std::string empty = agent::bar::gauge_bar(0.0, 4);
    ASSERT_EQ(empty, "\u2591\u2591\u2591\u2591");
    // Full bar is all full blocks (\u2588).
    std::string full = agent::bar::gauge_bar(1.0, 4);
    ASSERT_EQ(full, "\u2588\u2588\u2588\u2588");
    // Half of 4 cells: two full blocks then two empty.
    std::string half = agent::bar::gauge_bar(0.5, 4);
    ASSERT_EQ(half, "\u2588\u2588\u2591\u2591");
    // Degenerate width.
    ASSERT_EQ(agent::bar::gauge_bar(0.5, 0), "");
}

// The moving highlight: a soft band a third of the region wide, travelling from
// just outside the left edge to just outside the right one.
TEST(statusbar_shine_band_is_a_soft_third_of_the_region) {
    // 24 cells -> an 8-cell band. At half phase its centre is cell 12, so the
    // core covers 11-13 and the fade reaches 9 and 15.
    ASSERT_EQ(agent::bar::shine_level(12, 24, 0.5), 2);
    ASSERT_EQ(agent::bar::shine_level(11, 24, 0.5), 2);
    ASSERT_EQ(agent::bar::shine_level(13, 24, 0.5), 2);
    ASSERT_EQ(agent::bar::shine_level(10, 24, 0.5), 1);
    ASSERT_EQ(agent::bar::shine_level(9, 24, 0.5), 1);
    ASSERT_EQ(agent::bar::shine_level(15, 24, 0.5), 1);
    ASSERT_EQ(agent::bar::shine_level(8, 24, 0.5), 0);
    ASSERT_EQ(agent::bar::shine_level(16, 24, 0.5), 0);
}

// The gauge narrows its band to a three-cell glint, so the highlight stays a
// glint on a 24-cell bar instead of washing out a third of it. The verb keeps
// the proportional band.
TEST(statusbar_shine_band_width_can_be_fixed) {
    // 24 cells, a 3-cell band: cell 12 is the core, 11 and 13 the fade, and
    // nothing else lights up at the halfway point.
    ASSERT_EQ(agent::bar::shine_level(12, 24, 0.5, 3), 2);
    ASSERT_EQ(agent::bar::shine_level(11, 24, 0.5, 3), 1);
    ASSERT_EQ(agent::bar::shine_level(13, 24, 0.5, 3), 1);
    ASSERT_EQ(agent::bar::shine_level(10, 24, 0.5, 3), 0);
    ASSERT_EQ(agent::bar::shine_level(14, 24, 0.5, 3), 0);
}

TEST(statusbar_shine_fixed_band_keeps_its_width) {
    // A three-cell glint never breathes into two cells: in the middle of the
    // sweep exactly three cells are lit at every phase, one core and a fade on
    // each side.
    for (double phase = 0.2; phase <= 0.8; phase += 0.05) {
        int lit = 0;
        for (int c = 0; c < 24; ++c)
            lit += agent::bar::shine_level(c, 24, phase, 3) > 0 ? 1 : 0;
        ASSERT_EQ(lit, 3);
    }
}

TEST(statusbar_shine_solid_style_lights_only_core_shade) {
    // The gauge's glint is a single colour: three cells, all at the core level,
    // so there is no mid/fade cell to read as a second signal.
    int lit = 0;
    int fade = 0;
    for (int c = 0; c < 24; ++c) {
        const int level = agent::bar::shine_level(c, 24, 0.5, 3, agent::bar::ShineStyle::Solid);
        lit += level > 0 ? 1 : 0;
        fade += level == 1 ? 1 : 0;
    }
    ASSERT_EQ(lit, 3);
    ASSERT_EQ(fade, 0);
}

TEST(statusbar_shine_band_defaults_to_the_proportional_width) {
    ASSERT_EQ(agent::bar::shine_band(24), 8);
    ASSERT_EQ(agent::bar::shine_band(5), 3);
    // 0 asks for the proportional band, so both spellings agree.
    ASSERT_EQ(agent::bar::shine_level(9, 24, 0.5, 0), agent::bar::shine_level(9, 24, 0.5));
    // A band wider than the region is clamped to the region.
    ASSERT_EQ(agent::bar::shine_level(2, 4, 0.5, 9), 2);
}

TEST(statusbar_shine_band_travels_left_to_right) {
    int early = -1;
    int late = -1;
    for (int c = 0; c < 24 && (early < 0 || late < 0); ++c) {
        if (early < 0 && agent::bar::shine_level(c, 24, 0.3) > 0)
            early = c;
        if (late < 0 && agent::bar::shine_level(c, 24, 0.7) > 0)
            late = c;
    }
    ASSERT(early >= 0);
    ASSERT(late > early);
}

TEST(statusbar_shine_phase_is_periodic) {
    for (int c = 0; c < 24; ++c)
        ASSERT_EQ(agent::bar::shine_level(c, 24, 0.25), agent::bar::shine_level(c, 24, 1.25));
}

TEST(statusbar_shine_is_empty_without_cells) {
    ASSERT_EQ(agent::bar::shine_level(0, 0, 0.5), 0);
    ASSERT_EQ(agent::bar::shine_level(-1, 24, 0.5), 0);
    ASSERT_EQ(agent::bar::shine_level(24, 24, 0.5), 0);
}

// ---------------------------------------------------------------------------
// LLM streaming SSE parse (integration via a tiny in-process HTTP server)
// ---------------------------------------------------------------------------

namespace {
// Listen on loopback:port. Returns the listening fd, or -1 on failure.
int bind_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    listen(fd, 1);
    return fd;
}

// Read one full HTTP request (headers + Content-Length body) from a client.
std::string drain_request(int c) {
    char buf[4096];
    std::string req;
    while (true) {
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            break;
        req.append(buf, n);
        if (req.find("\r\n\r\n") == std::string::npos)
            continue;
        const size_t cl = req.find("Content-Length:");
        if (cl == std::string::npos)
            break;
        const size_t hl = req.find("\r\n\r\n");
        long len = std::atol(req.c_str() + cl + 15);
        while ((long)req.size() < (long)hl + 4 + len) {
            int m = recv(c, buf, sizeof(buf) - 1, 0);
            if (m <= 0)
                break;
            req.append(buf, m);
        }
        break;
    }
    return req;
}

// Serve `connections` canned HTTP responses (any status/content-type/payload),
// then close. Lets the wire-layer pins exercise buffered chat, HTTP-error and
// streaming paths without any external dependency.
//
// Bounded on purpose: the thread exits after the agreed number of accepts, so
// nothing outlives the fd the test closes. An unbounded loop (or a poll that
// keeps watching the fd number) would answer a later test's connection once the
// fd is reused. A code path that makes more requests than the mock serves would
// block in read() — the harness watchdog names that test instead of hanging.
int spawn_mock_http(int port, std::string& body_out, const std::string& status,
                    const std::string& content_type, const std::string& payload,
                    int connections = 1) {
    int fd = bind_listener(port);
    if (fd < 0)
        return -1;
    body_out.clear();
    std::thread t([fd, status, content_type, payload, &body_out, connections]() {
        for (int i = 0; i < connections; ++i) {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0)
                return;
            std::string request = drain_request(c);
            if (i == 0)
                body_out = std::move(request); // the first request is the pinned one
            std::string http = status + "\r\nContent-Type: " + content_type +
                               "\r\nContent-Length: " + std::to_string(payload.size()) +
                               "\r\n\r\n" + payload;
            send(c, http.c_str(), http.size(), 0);
            // give client time to read
            usleep(200000);
            close(c);
        }
    });
    t.detach();
    return fd;
}

// Serve a scripted sequence of fully-formed HTTP responses, one per accepted
// connection. The first request is captured for wire assertions.
int spawn_mock_http_sequence(int port, std::string& first_request,
                             const std::vector<std::string>& responses) {
    int fd = bind_listener(port);
    if (fd < 0)
        return -1;
    first_request.clear();
    std::thread t([fd, responses, &first_request]() {
        for (std::size_t i = 0; i < responses.size(); ++i) {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0)
                return;
            std::string request = drain_request(c);
            if (i == 0)
                first_request = std::move(request);
            send(c, responses[i].data(), responses[i].size(), 0);
            usleep(50000);
            close(c);
        }
    });
    t.detach();
    return fd;
}

// Serve one canned SSE response (a streamed tool call in two fragments by
// default), then close. Lets us exercise LLMClient::chat_stream including
// fragment merging without any external dependency.
int spawn_mock_sse(int port, std::string& body_out, const std::string& sse_override = "") {
    std::string sse =
        !sse_override.empty()
            ? sse_override
            : std::string("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
                          "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
                          "\"arguments\":\"\"}}]}}]}\n\n"
                          "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                          "\"function\":{\"arguments\":\"{\\\"pattern\\\":\\\"foo\\\",\\\"path\\\":"
                          "\\\".\\\"}\"}}]}}]}\n\n"
                          "data: [DONE]\n\n");
    return spawn_mock_http(port, body_out, "HTTP/1.1 200 OK", "text/event-stream", sse);
}

// Accept one request, drain it, then hold the connection open WITHOUT
// responding. An in-flight LLM call against this server blocks until the
// caller's cancel token aborts it (curl's progress callback polls the token).
int spawn_stall_server(int port) {
    int fd = bind_listener(port);
    if (fd < 0)
        return -1;
    std::thread t([fd]() {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0)
            return;
        drain_request(c);
        sleep(30); // hold open; the test cancels well before this expires
        close(c);
    });
    t.detach();
    return fd;
}

// Serve every connection after a fixed hold and count the requests. Lets the
// single-flight tests observe whether concurrent fetchers issued duplicate
// HTTP requests (hits > 1 means coalescing failed).
int spawn_counting_slow_mock(int port, std::atomic<int>& hits, int hold_ms,
                             const std::string& payload) {
    int fd = bind_listener(port);
    if (fd < 0)
        return -1;
    std::thread([fd, &hits, hold_ms, payload]() {
        while (true) {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0)
                return;
            std::thread([c, &hits, hold_ms, payload]() {
                drain_request(c);
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
                std::string http = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                   "Content-Length: " +
                                   std::to_string(payload.size()) + "\r\n\r\n" + payload;
                send(c, http.c_str(), http.size(), 0);
                ++hits;
                usleep(50000);
                close(c);
            }).detach();
        }
    }).detach();
    return fd;
}
} // namespace

using agent::test::XdgGuard;

// ---------------------------------------------------------------------------
// Private file modes (#373)
// ---------------------------------------------------------------------------

namespace {

mode_t mode_of(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return st.st_mode & 0777;
}

struct UmaskGuard {
    explicit UmaskGuard(mode_t mask) : old(::umask(mask)) {}
    ~UmaskGuard() { ::umask(old); }
    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;
    mode_t old;
};

} // namespace

TEST(private_provider_file_is_owner_only) {
    XdgGuard xdg("private_provider");
    UmaskGuard umask(022);
    auto repo = agent::make_file_provider_repository();
    agent::Provider p;
    p.name = "private-test";
    p.api_base = "https://example.test/v1";
    p.api_key = "secret";
    ASSERT_TRUE(repo->save(p));
    const std::string dir = agent::global_config_dir() + "/providers";
    ASSERT_EQ(mode_of(dir), 0700u);
    ASSERT_EQ(mode_of(dir + "/private-test.conf"), 0600u);
}

TEST(private_global_config_is_owner_only) {
    XdgGuard xdg("private_config");
    UmaskGuard umask(022);
    agent::Config c;
    c.provider_name = "openai";
    c.api_key = "secret";
    ASSERT_TRUE(c.save_global(agent::global_config_path()));
    ASSERT_EQ(mode_of(agent::global_config_path()), 0600u);
}

TEST(private_bootstrap_config_is_owner_only) {
    XdgGuard xdg("private_bootstrap");
    UmaskGuard umask(022);
    ASSERT_TRUE(agent::ensure_global_config());
    ASSERT_EQ(mode_of(agent::global_config_path()), 0600u);
}

TEST(private_session_file_is_owner_only) {
    XdgGuard xdg("private_session");
    UmaskGuard umask(022);
    agent::SessionStore store;
    agent::Session s;
    s.id = "private-session";
    s.title = "private";
    ASSERT_TRUE(store.save(s));
    ASSERT_EQ(mode_of(store.dir()), 0700u);
    ASSERT_EQ(mode_of(store.dir() + "/private-session.json"), 0600u);
}

TEST(private_transcript_is_owner_only) {
    XdgGuard xdg("private_log");
    UmaskGuard umask(022);
    const std::string dir = xdg.state + "/logs";
    {
        agent::ConversationLog log;
        log.open(dir + "/{ts}.jsonl");
        ASSERT_TRUE(log.enabled());
        log.event("user", {{"content", "secret"}});
    }
    std::string file;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".jsonl")
            file = entry.path().string();
    }
    ASSERT_TRUE(!file.empty());
    ASSERT_EQ(mode_of(dir), 0700u);
    ASSERT_EQ(mode_of(file), 0600u);
}

TEST(private_error_journal_is_owner_only) {
    XdgGuard xdg("private_error_log");
    UmaskGuard umask(022);
    const std::string path = xdg.state + "/errors/errors.jsonl";
    {
        agent::ConversationLog log;
        log.open_errors(path);
        ASSERT_TRUE(log.errors_enabled());
        log.event("error", {{"message", "secret"}});
    }
    ASSERT_EQ(mode_of(xdg.state + "/errors"), 0700u);
    ASSERT_EQ(mode_of(path), 0600u);
}

TEST(global_selection_is_what_a_live_run_uses) {
    // A live run must use the configuration the user actually runs. The provider
    // selected in the global config decides the endpoint and the key; a cloned
    // repository's amber.conf is a base layer and cannot redirect that traffic to
    // a local server. The benchmark used to load the project file directly, so a
    // run against the configured provider silently dialled 127.0.0.1.
    XdgGuard xdg("global_selection");
    std::filesystem::create_directories(agent::global_config_dir() + "/providers");
    {
        std::ofstream f(agent::global_config_path());
        f << "provider=kilocode\nmodel=kilo-auto/free\nmodel_explicit=1\n";
    }
    {
        // What the provider domain reads before any plugin is installed: the
        // saved provider file owns the endpoint and the key.
        std::ofstream f(agent::global_config_dir() + "/providers/kilocode.conf");
        f << "provider=kilocode\napi_base=https://api.kilo.ai/api/gateway\n"
             "api_key=test-key\ndefault_model=kilo-auto/free\n";
    }
    const std::string project = xdg.state + "/project-amber.conf";
    std::filesystem::create_directories(xdg.state);
    {
        std::ofstream f(project);
        f << "api_base=http://127.0.0.1:8081/v1\n";
    }

    agent::Config cfg;
    const agent::GlobalConfig global = agent::apply_global_selection(cfg);
    ASSERT(global.loaded);
    ASSERT_EQ(cfg.provider_name, std::string("kilocode"));
    ASSERT(cfg.api_base.find("kilo.ai") != std::string::npos);
    ASSERT(cfg.api_base_from_provider);

    // The project layer still applies, but not its endpoint.
    cfg.load_workspace_defaults(project);
    ASSERT(cfg.api_base.find("127.0.0.1") == std::string::npos);

    agent::apply_global_model(cfg, global);
    ASSERT_EQ(cfg.model, std::string("kilo-auto/free"));
    ASSERT(cfg.model_explicit);

    // No global config: nothing is imposed, so a bare host keeps its own defaults.
    std::filesystem::remove(agent::global_config_path());
    agent::Config bare;
    const agent::GlobalConfig none = agent::apply_global_selection(bare);
    ASSERT(!none.loaded);
    ASSERT(!bare.api_base_from_provider);
    ASSERT_EQ(bare.provider_name, std::string("custom")); // core's default, untouched
}

TEST(private_migration_narrows_existing_paths_and_is_idempotent) {
    XdgGuard xdg("private_migration");
    UmaskGuard umask(022);
    const std::string config = agent::global_config_path();
    const std::string providers = agent::global_config_dir() + "/providers";
    const std::string state = agent::Workspace::state_dir();
    const std::string sessions = state + "/sessions";
    const std::string session = sessions + "/old.json";
    std::filesystem::create_directories(providers);
    std::filesystem::create_directories(sessions);
    {
        std::ofstream f(config);
        f << "api_key=secret\n";
    }
    {
        std::ofstream f(providers + "/old.conf");
        f << "api_key=secret\n";
    }
    {
        std::ofstream f(session);
        f << "{}\n";
    }
    ASSERT_EQ(::chmod(config.c_str(), 0644), 0);
    ASSERT_EQ(::chmod((providers + "/old.conf").c_str(), 0644), 0);
    ASSERT_EQ(::chmod(session.c_str(), 0644), 0);

    agent::secure_private_state();
    agent::secure_private_state(); // idempotent

    ASSERT_EQ(mode_of(config), 0600u);
    ASSERT_EQ(mode_of(providers), 0700u);
    ASSERT_EQ(mode_of(providers + "/old.conf"), 0600u);
    ASSERT_EQ(mode_of(state), 0700u);
    ASSERT_EQ(mode_of(sessions), 0700u);
    ASSERT_EQ(mode_of(session), 0600u);
}

TEST(private_migration_does_not_follow_symlinks) {
    XdgGuard xdg("private_symlink");
    UmaskGuard umask(022);
    std::filesystem::create_directories(xdg.dir);
    const std::string target = xdg.dir + "/outside.conf";
    {
        std::ofstream f(target);
        f << "api_key=secret\n";
    }
    ASSERT_EQ(::chmod(target.c_str(), 0644), 0);
    std::filesystem::create_directories(agent::global_config_dir());
    ASSERT_EQ(::symlink(target.c_str(), agent::global_config_path().c_str()), 0);

    agent::secure_private_state();

    ASSERT_EQ(mode_of(target), 0644u);
    ASSERT_TRUE(std::filesystem::is_symlink(agent::global_config_path()));
}

// ---------------------------------------------------------------------------
// Tool-result artifacts (#344 P2)
// ---------------------------------------------------------------------------

TEST(artifacts_store_and_slice_a_payload) {
    XdgGuard xdg("artifacts_store");
    std::string payload;
    for (int i = 0; i < 2000; ++i)
        payload += "line " + std::to_string(i) + "\n";
    const std::string id = agent::store_tool_artifact(payload);
    ASSERT_EQ(id.size(), 16u);
    ASSERT(agent::valid_artifact_id(id));
    std::string error;
    const auto head = agent::read_tool_artifact(id, 0, 40, "", error);
    ASSERT(head.has_value());
    ASSERT_EQ(*head, payload.substr(0, 40));
    const auto mid = agent::read_tool_artifact(id, 100, 20, "", error);
    ASSERT(mid.has_value());
    ASSERT_EQ(*mid, payload.substr(100, 20));
    ASSERT(error.empty());
}

TEST(artifacts_grep_returns_numbered_matches) {
    XdgGuard xdg("artifacts_grep");
    std::string payload = "alpha\nneedle one\nbeta\nneedle two\n";
    payload += std::string(5000, 'z'); // above the store threshold
    const std::string id = agent::store_tool_artifact(payload);
    ASSERT(!id.empty());
    std::string error;
    const auto hits = agent::read_tool_artifact(id, 0, 4096, "needle", error);
    ASSERT(hits.has_value());
    ASSERT(hits->find("2:needle one") != std::string::npos);
    ASSERT(hits->find("4:needle two") != std::string::npos);
    ASSERT(hits->find("alpha") == std::string::npos);
    const auto none = agent::read_tool_artifact(id, 0, 4096, "absent-token", error);
    ASSERT(!none.has_value());
    ASSERT(error.find("no matches") != std::string::npos);
}

TEST(artifacts_reject_invalid_and_expired_ids) {
    XdgGuard xdg("artifacts_reject");
    std::string error;
    ASSERT(!agent::read_tool_artifact("../../etc/passwd", 0, 10, "", error).has_value());
    ASSERT(!agent::valid_artifact_id("../../etc/passwd"));
    ASSERT(!agent::valid_artifact_id("0123456789abcdef0"));
    error.clear();
    const auto missing = agent::read_tool_artifact("0000000000000000", 0, 10, "", error);
    ASSERT(!missing.has_value());
    ASSERT(error.find("unknown") != std::string::npos);
}

TEST(artifacts_retention_evicts_the_oldest) {
    XdgGuard xdg("artifacts_retention");
    std::vector<std::string> ids;
    for (int i = 0; i < 70; ++i) {
        const std::string id =
            agent::store_tool_artifact(std::string(4096, static_cast<char>('a' + i % 26)));
        ASSERT(!id.empty());
        ids.push_back(id);
    }
    std::string error;
    ASSERT(!agent::read_tool_artifact(ids.front(), 0, 1, "", error).has_value());
    ASSERT(agent::read_tool_artifact(ids.back(), 0, 1, "", error).has_value());
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(agent::artifact_dir()))
        if (entry.is_regular_file())
            ++files;
    ASSERT(files <= agent::kArtifactMaxFiles);
}

TEST(artifacts_skip_payloads_below_the_threshold) {
    XdgGuard xdg("artifacts_threshold");
    ASSERT_EQ(agent::store_tool_artifact("tiny"), std::string());
}

TEST(read_result_tool_reads_a_stored_slice) {
    XdgGuard xdg("read_result_tool");
    const std::string payload = std::string(9000, 'r') + "TAIL";
    const std::string id = agent::store_tool_artifact(payload);
    ASSERT(!id.empty());
    const auto tool = agent::make_read_result_tool();
    ASSERT_EQ(tool->name(), "read_result");
    ASSERT(tool->is_read_only());
    ASSERT(!tool->requires_approval(json::object()));
    const agent::ToolResult ok = tool->execute({{"result_id", id}, {"offset", 0}, {"limit", 8}});
    ASSERT(ok.ok);
    ASSERT_EQ(ok.output, payload.substr(0, 8));
    const agent::ToolResult missing = tool->execute({{"offset", 0}});
    ASSERT(!missing.ok);
    ASSERT(missing.error.find("result_id") != std::string::npos);
}

TEST(tool_envelope_marker_names_the_result_handle) {
    agent::ToolResult big;
    big.output.assign(9000, 'k');
    big.meta = {{"result_id", "0123456789abcdef"}};
    const std::string env = agent::format_tool_envelope("read", json::object(), big, 4096);
    ASSERT(env.find("read_result result_id=0123456789abcdef") != std::string::npos);
}

// Router-style /models listing (kilocode et al.): kilo-auto/frontier (1M)
// is listed BEFORE kilo-auto/free (256k). The autodetect probe must adopt the
// ACTIVE model's window (the user's explicit model), not the first entry with
// a context window — the wrong-model adoption sized the gauge and the
// compression budget to 1M while running kilo-auto/free.
TEST(probe_autodetect_prefers_explicit_active_model) {
    XdgGuard xdg("autodetect_explicit");
    std::string dummy;
    int srv = spawn_mock_sse(8924, dummy, R"({"data":[
        {"id":"kilo-auto/frontier","object":"model","owned_by":"kilo",
         "context_length":1000000,"meta":null},
        {"id":"kilo-auto/free","object":"model","owned_by":"kilo",
         "context_length":256000,"meta":null}]})");
    ASSERT(srv >= 0);
    usleep(100000); // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8924/v1";
    cfg.model = "kilo-auto/free";
    cfg.model_explicit = true;
    agent::apply_server_autodetect(cfg);
    close(srv);

    ASSERT_EQ(cfg.context_size, 256000);
    ASSERT_EQ(cfg.model, "kilo-auto/free");
}

// With no explicit model the autodetect keeps its documented behavior: the
// first entry reporting a window wins (auto-detect mode picks the server's
// lead model, e.g. llama.cpp's single listing).
TEST(probe_autodetect_first_with_context_when_auto) {
    XdgGuard xdg("autodetect_auto");
    std::string dummy;
    int srv = spawn_mock_sse(8925, dummy, R"({"data":[
        {"id":"kilo-auto/frontier","object":"model","owned_by":"kilo",
         "context_length":1000000,"meta":null},
        {"id":"kilo-auto/free","object":"model","owned_by":"kilo",
         "context_length":256000,"meta":null}]})");
    ASSERT(srv >= 0);
    usleep(100000); // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8925/v1";
    cfg.model_explicit = false; // auto-detect
    agent::apply_server_autodetect(cfg);
    close(srv);

    ASSERT_EQ(cfg.context_size, 1000000);
    ASSERT_EQ(cfg.model, "kilo-auto/frontier");
}

// ---------------------------------------------------------------------------
// Model catalog cache: stale-while-revalidate, single-flight, disk-only reads
// ---------------------------------------------------------------------------

TEST(model_catalog_freshness_ttl) {
    agent::ModelCatalogEntry e;
    e.body = "{}";
    e.fetched_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    ASSERT(agent::model_catalog_fresh(e));
    e.fetched_ms -= 25LL * 3600 * 1000;
    ASSERT_FALSE(agent::model_catalog_fresh(e));
}

// The startup path's only data source is the disk catalog: every cache-only
// read must return without touching the network. The stall server accepts
// the connection and never replies, so any real fetch would burn the full
// 10s curl timeout — the bound below is what the UI main thread can afford.
TEST(model_catalog_reads_are_disk_only) {
    XdgGuard xdg("catalog_diskonly");
    int srv = spawn_stall_server(8926);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8926/v1";
    auto t0 = std::chrono::steady_clock::now();
    ASSERT_FALSE(agent::model_catalog_read(cfg).has_value());
    ASSERT_FALSE(agent::probe_server_cached(cfg).ok);
    ASSERT(agent::list_model_info_cached(cfg).empty());
    agent::Config c2 = cfg;
    ASSERT_FALSE(agent::apply_cached_server_autodetect(c2).ok);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT(ms < 1000);
    close(srv);
}

TEST(model_catalog_stale_serve_and_stale_if_error) {
    XdgGuard xdg("catalog_stale");
    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:1/v1"; // nothing listens: refused instantly
    const std::string body = R"({"data":[{"id":"m1","context_length":4096}]})";

    agent::model_catalog_write(cfg, body);
    auto e = agent::model_catalog_read(cfg);
    ASSERT(e.has_value());
    ASSERT_EQ(e->body, body);
    ASSERT(agent::model_catalog_fresh(*e));

    // Fresh cache: the cache-through fetch serves disk without the network.
    auto r = agent::model_catalog_fetch(cfg);
    ASSERT(r.has_value());
    ASSERT_EQ(r->entry.body, body);
    ASSERT_FALSE(r->fetched);

    // Age the entry past the TTL by rewriting its timestamp in place.
    std::string cache_file;
    for (const auto& de : std::filesystem::directory_iterator(xdg.cache + "/amber"))
        cache_file = de.path().string();
    ASSERT(!cache_file.empty());
    {
        std::ofstream f(cache_file, std::ios::trunc);
        f << agent::json{{"fetched_ms", 0}, {"body", body}}.dump();
    }
    e = agent::model_catalog_read(cfg);
    ASSERT(e.has_value());
    ASSERT_FALSE(agent::model_catalog_fresh(*e));

    // Stale is still served cache-through (SWR: revalidation is async).
    r = agent::model_catalog_fetch(cfg);
    ASSERT(r.has_value());
    ASSERT_EQ(r->entry.body, body);

    // Forced revalidation fails (refused) but the stale entry survives
    // (stale-if-error).
    r = agent::model_catalog_fetch(cfg, /*force=*/true);
    ASSERT(r.has_value());
    ASSERT_EQ(r->entry.body, body);
    ASSERT_FALSE(r->fetched);
}

// A failed refresh must be able to say WHY: the endpoint it asked and the
// status that came back. Before this, /get model list printed "refresh running
// in background" even after its fetch had 404'd, which is how a base pasted
// from a provider's docs (giving /v1/v1/models) read as "no models exist".
TEST(catalog_failure_reason_names_the_endpoint_and_status) {
    agent::CatalogFetchReport r;
    r.url = "https://api.anthropic.com/models";
    r.http_code = 404;
    // With FAILONERROR a 4xx arrives as a transport error; the status is still
    // known, and "no answer" for a 404 is exactly the misleading reading this
    // exists to kill.
    r.transport_ok = false;
    const std::string why = r.failure_reason();
    ASSERT(why.find("https://api.anthropic.com/models") != std::string::npos);
    ASSERT(why.find("HTTP 404") != std::string::npos);
    ASSERT(why.find("no answer") == std::string::npos);

    r.http_code = 401;
    ASSERT(r.failure_reason().find("rejected the key") != std::string::npos);

    r.http_code = 0;
    ASSERT(r.failure_reason().find("no answer") != std::string::npos);

    r.fetched = true;
    ASSERT(r.failure_reason().empty());

    agent::CatalogFetchReport no_endpoint;
    ASSERT(no_endpoint.failure_reason().find("no model-listing endpoint") != std::string::npos);
}

// The background refresh writes the catalog and reports through the host's
// post queue; the fresh entry then serves every cache-only read.
TEST(model_catalog_refresh_async_populates_cache) {
    XdgGuard xdg("catalog_async");
    std::string dummy;
    const std::string body = R"({"data":[{"id":"m1","context_length":4096}]})";
    int srv = spawn_mock_sse(8927, dummy, body);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8927/v1";

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::function<void()>> posted;
    bool done_fetched = false;
    bool done_called = false;
    auto post = [&](std::function<void()> fn) {
        std::scoped_lock lk(mtx);
        posted.push_back(std::move(fn));
        cv.notify_all();
    };
    auto done = [&](const agent::CatalogFetchReport& report) {
        std::scoped_lock lk(mtx);
        done_called = true;
        done_fetched = report.fetched;
        cv.notify_all();
    };

    agent::model_catalog_refresh_async(cfg, post, done);

    // Pump the posted work like the UI event loop would.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock lk(mtx);
            cv.wait_until(lk, deadline, [&] { return !posted.empty() || done_called; });
            if (posted.empty())
                break;
            fn = std::move(posted.front());
            posted.erase(posted.begin());
        }
        fn();
    }
    ASSERT(done_called);
    ASSERT(done_fetched);
    close(srv);

    auto e = agent::model_catalog_read(cfg);
    ASSERT(e.has_value());
    ASSERT_EQ(e->body, body);
    auto infos = agent::list_model_info_cached(cfg);
    ASSERT_EQ(infos.size(), (size_t)1);
    ASSERT_EQ(infos[0].id, "m1");
    ASSERT_EQ(infos[0].context, 4096);
}

// Concurrent forced fetches for one endpoint must share a single HTTP
// request — the slow mock keeps the first fetch in flight long enough for
// the joiners to arrive, so hits == 1 is deterministic.
TEST(model_catalog_singleflight_coalesces_concurrent_fetches) {
    XdgGuard xdg("catalog_singleflight");
    std::atomic<int> hits{0};
    const std::string body = R"({"data":[{"id":"m1","context_length":4096}]})";
    int srv = spawn_counting_slow_mock(8928, hits, 400, body);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8928/v1";

    constexpr int kCallers = 4;
    std::vector<std::optional<agent::CatalogFetchResult>> results(kCallers);
    std::atomic<int> ready{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kCallers; ++i)
        ts.emplace_back([&, i] {
            ++ready;
            while (ready.load() < kCallers)
                std::this_thread::yield();
            results[i] = agent::model_catalog_fetch(cfg, /*force=*/true);
        });
    for (auto& t : ts)
        t.join();
    close(srv);

    ASSERT_EQ(hits.load(), 1);
    for (const auto& r : results) {
        ASSERT(r.has_value());
        ASSERT_EQ(r->entry.body, body);
    }
}

TEST(llm_streaming_tool_call_object_arguments_preserved) {
    // Some OpenAI-compatible servers stream tool-call `arguments` as a JSON
    // object in one delta instead of a string fragment. The parser must
    // preserve it, otherwise a valid call (e.g. search with a pattern) arrives
    // at the tool as `{}` and errors ("missing 'pattern'").
    agent::Message m;
    auto sink = [](const agent::StreamChunk&) {};
    auto p = agent::make_dialect("openai")->make_decoder(m, sink, "");

    auto ev = [](const agent::json& tc) -> std::string {
        agent::json delta = {{"tool_calls", tc}};
        agent::json choice = {{"delta", delta}};
        return "data: " + agent::json{{"choices", agent::json::array({choice})}}.dump() + "\n\n";
    };

    // Fragment 1: arguments as a JSON object (not a string).
    agent::json fn1 = {{"name", "search"},
                       {"arguments", agent::json::object({{"pattern", "ncurses"}})}};
    agent::json call1 = {{"index", 0}, {"id", "c1"}, {"type", "function"}, {"function", fn1}};
    std::string s1 = ev(agent::json::array({call1}));

    // Fragment 2: a trailing string fragment appended to the object.
    agent::json fn2 = {{"arguments", "|curses"}};
    agent::json call2 = {{"index", 0}, {"function", fn2}};
    std::string s2 = ev(agent::json::array({call2}));

    p->on_write(s1.c_str(), s1.size(), 1);
    p->on_write(s2.c_str(), s2.size(), 1);
    p->finalize();

    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    // The OpenAI wire contract requires `arguments` to be a JSON *string*; an
    // object fragment must be merged and re-serialized, never stored as a raw
    // object (object-typed arguments sent back to the API corrupt the turn).
    agent::json args = m.tool_calls[0]["function"]["arguments"];
    ASSERT_TRUE(args.is_string());
    agent::json parsed = agent::json::parse(args.get<std::string>(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed["pattern"], "ncurses");
}

// The stream decoder must survive being constructed with a plain `auto` lambda
// sink. The constructor binds its ChunkSink member to the caller's sink
// object; an `auto` lambda converts to a temporary std::function, so the
// member must store a COPY, never a reference — otherwise the first content
// delta (which invokes the sink) reads a dangling std::function
// (stack-use-after-scope, ASan-visible, crash under -O2). Found by the
// harness parse probe.
TEST(llm_streaming_parser_accepts_auto_lambda_sink) {
    agent::Message m;
    auto sink = [](const agent::StreamChunk&) {};
    auto p = agent::make_dialect("openai")->make_decoder(m, sink, "");
    const char* sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
                      "data: [DONE]\n\n";
    const std::string body(sse);
    p->on_write(body.c_str(), body.size(), 1);
    p->finalize();
    ASSERT_EQ(m.content, "hello");
}

// A 429 with Retry-After is a pause at the transport, not a terminal reply:
// the governor waits the header and the retry loop probes once. This is the
// bridge the per-turn unit tests cannot cover on their own.
TEST(http_429_honours_retry_after_and_retries_once) {
    std::string request;
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"recovered"}}],)"
        R"("usage":{"prompt_tokens":1,"completion_tokens":1}})";
    std::vector<std::string> responses = {
        "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body};
    int srv = spawn_mock_http_sequence(8962, request, responses);
    ASSERT(srv >= 0);
    usleep(100000); // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8962/v1";
    cfg.model = "mock";
    cfg.flavor = "openai";
    cfg.stream = false;
    agent::HttpLLMClient client(cfg);
    std::vector<agent::Message> msgs;
    agent::Message user;
    user.role = "user";
    user.content = "hi";
    msgs.push_back(user);

    agent::AgentHooks hooks;
    agent::ConversationLog log;
    agent::CancellationToken token;
    auto chat = [&]() { return client.chat(msgs, {}, nullptr); };
    const auto begin = std::chrono::steady_clock::now();
    const agent::Message reply = agent::chat_with_retry(hooks, log, chat, "test", token, 3, {});
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    ASSERT_EQ(reply.content, "recovered");
    ASSERT(waited.count() >= 900);
    ASSERT(request.find("POST /v1/chat/completions") != std::string::npos);
    close(srv);
}

TEST(llm_streaming_merges_tool_call_fragments) {
    std::string dummy;
    int srv = spawn_mock_sse(8911, dummy);
    ASSERT(srv >= 0);
    usleep(100000); // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8911/v1";
    cfg.stream = true;
    agent::HttpLLMClient client(cfg);

    std::vector<std::string> tokens;
    agent::Message m = client.chat_stream({}, {}, [&tokens](const agent::StreamChunk& ch) {
        if (!ch.done && !ch.delta.empty())
            tokens.push_back(ch.delta);
    });

    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "search");
    std::string args = m.tool_calls[0]["function"]["arguments"].get<std::string>();
    agent::json parsed = agent::json::parse(args, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed["pattern"], "foo");
    ASSERT_EQ(parsed["path"], ".");
    close(srv);
}

TEST(llm_streaming_inline_think_segmentation) {
    std::string dummy;
    // Content stream with inline <think> spanning fragments; answer follows.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<thi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"nk>plan the\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" answer</think>Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8912, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8912/v1";
    cfg.stream = true;
    agent::HttpLLMClient client(cfg);

    std::string answer, reasoning;
    agent::Message m = client.chat_stream({}, {}, [&](const agent::StreamChunk& ch) {
        if (ch.done)
            return;
        answer += ch.delta;
        reasoning += ch.reasoning;
    });

    ASSERT_EQ(m.content, "Hello world");
    ASSERT_EQ(m.reasoning, "plan the answer");
    ASSERT_EQ(answer, "Hello world");
    ASSERT_EQ(reasoning, "plan the answer");
    close(srv);
}

TEST(llm_streaming_reasoning_content_field) {
    std::string dummy;
    // Dedicated reasoning_content field (vLLM / llama.cpp deepseek format).
    std::string sse = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step one \"}}]}\n\n"
                      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step two\"}}]}\n\n"
                      "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
                      "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8913, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8913/v1";
    cfg.stream = true;
    agent::HttpLLMClient client(cfg);

    agent::Message m = client.chat_stream({}, {}, [](const agent::StreamChunk&) {});

    ASSERT_EQ(m.content, "done");
    ASSERT_EQ(m.reasoning, "step one step two");
    close(srv);
}

TEST(llm_streaming_captures_usage_stats) {
    std::string dummy;
    // Final include_usage chunk: usage present, empty choices[] (llama.cpp/vLLM).
    std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                      "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4096,"
                      "\"completion_tokens\":128,\"total_tokens\":4224}}\n\n"
                      "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8914, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8914/v1";
    cfg.stream = true;
    agent::HttpLLMClient client(cfg);

    agent::Stats stats;
    agent::Message m = client.chat_stream({}, {}, [](const agent::StreamChunk&) {}, &stats);

    ASSERT_EQ(m.content, "hi");
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.prompt_tokens, 4096L);
    ASSERT_EQ(stats.completion_tokens, 128L);
    ASSERT_TRUE(stats.latency_ms >= 0);
    close(srv);
}

// ---------------------------------------------------------------------------
// Wire-layer characterization pins (FIX-027): lock the current OBSERVABLE
// behavior of the HTTP/SSE layer before the dialect refactor (FIX-028) moves
// it. Each test documents today's contract; the pure-move refactor must keep
// every assertion true with only mechanical call-site renames.
// ---------------------------------------------------------------------------

TEST(apply_auth_emits_bearer_only_with_key) {
    agent::Config cfg; // no api_key
    agent::HeaderList headers;
    for (const auto& h : agent::make_dialect("openai")->auth_headers(cfg))
        headers.add(h);
    ASSERT(headers.list == nullptr);

    cfg.api_key = "sk-test-123";
    agent::HeaderList keyed;
    for (const auto& h : agent::make_dialect("openai")->auth_headers(cfg))
        keyed.add(h);
    ASSERT(keyed.list != nullptr);
    ASSERT_EQ(std::string(keyed.list->data), "Authorization: Bearer sk-test-123");
    ASSERT(keyed.list->next == nullptr); // never more than the one auth header
}

TEST(config_models_url_derivation) {
    agent::Config c;
    auto d = agent::make_dialect("openai");
    ASSERT_EQ(d->models_url(c), "http://localhost:8000/v1/models");
    c.api_base = "https://api.example.com/v1";
    ASSERT_EQ(d->models_url(c), "https://api.example.com/v1/models");
}

// The 400-overflow prose sniffer is the runtime truth for the context window:
// each pattern family must keep working across the dialect move.
TEST(dialect_context_overflow_hint_patterns) {
    auto d = agent::make_dialect("openai");
    ASSERT_EQ(d->context_overflow_hint("maximum context length is 8192 tokens"), 8192);
    ASSERT_EQ(d->context_overflow_hint("max context length: 4096"), 4096);
    ASSERT_EQ(d->context_overflow_hint("max context length is 2048"), 2048);
    ASSERT_EQ(d->context_overflow_hint("n_ctx is 2048"), 2048);
    ASSERT_EQ(d->context_overflow_hint("n_ctx = 1024"), 1024);
    ASSERT_EQ(d->context_overflow_hint("context length exceeds 16384"), 16384);
    ASSERT_EQ(d->context_overflow_hint("Request exceeds maximum context length (4096 tokens)"),
              4096);
    ASSERT_EQ(d->context_overflow_hint("context length of 32768"), 32768);
    // OpenAI wraps the prose in an error object; the sniffer scans the body.
    ASSERT_EQ(
        d->context_overflow_hint(
            R"({"error":{"message":"This model's maximum context length is 16384 tokens."}})"),
        16384);
    // No known pattern, or an implausible value, yields 0 (never a guess).
    ASSERT_EQ(d->context_overflow_hint(R"({"error":"bad request"})"), 0);
    ASSERT_EQ(d->context_overflow_hint(""), 0);
    ASSERT_EQ(d->context_overflow_hint("maximum context length is 999999999 tokens"), 0);
}

TEST(buffered_chat_fills_stats_from_usage) {
    // The buffered path's telemetry contract: usage.prompt_tokens /
    // completion_tokens must land in Stats (mirrors the streamed path).
    std::string req;
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello "
        "there\"}}],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":3,"
        "\"total_tokens\":53}}";
    int srv = spawn_mock_http(8931, req, "HTTP/1.1 200 OK", "application/json", body);
    ASSERT(srv >= 0);
    usleep(100000); // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8931/v1";
    agent::HttpLLMClient client(cfg);
    agent::Stats stats;
    agent::Message m = client.chat({}, {}, &stats);

    ASSERT_EQ(m.role, "assistant");
    ASSERT_EQ(m.content, "hello there");
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.prompt_tokens, 50L);
    ASSERT_EQ(stats.completion_tokens, 3L);
    ASSERT(stats.latency_ms >= 0);
    close(srv);
}

TEST(buffered_chat_malformed_body_degrades_to_recovery_message) {
    // A 200 whose body is not a chat-completions JSON (here: an SSE-shaped
    // stream served to the BUFFERED path) must degrade into an assistant
    // message carrying the raw body — which the agent loop feeds back to the
    // model for self-recovery — never an exception.
    std::string dummy;
    int srv = spawn_mock_sse(8932, dummy); // default SSE tool-call stream
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8932/v1";
    agent::HttpLLMClient client(cfg);
    agent::Message m = client.chat({}, {}, nullptr);

    ASSERT_EQ(m.role, "assistant");
    const std::string prefix = "[error: malformed LLM response, raw body follows]";
    ASSERT_EQ(m.content.compare(0, prefix.size(), prefix), 0);
    ASSERT(m.content.find("data:") != std::string::npos); // raw body kept
    close(srv);
}

TEST(overflow_400_streaming_throws_non_retryable_api_error) {
    // llama.cpp-style plain-text overflow rejection on the streaming path.
    // The client must throw a typed, non-retryable ApiError (never a fake
    // assistant reply, never a retry loop).
    std::string req;
    int srv = spawn_mock_http(8933, req, "HTTP/1.1 400 Bad Request", "text/plain", "n_ctx is 2048");
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8933/v1";
    agent::HttpLLMClient client(cfg);
    bool threw = false;
    try {
        client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
    } catch (const agent::ApiError& e) {
        threw = true;
        ASSERT_EQ(e.status, 400L);
        ASSERT_FALSE(e.retryable);
        ASSERT_EQ(std::string(e.what()).find("HTTP 400 from LLM server"), 0u);
    }
    ASSERT(threw);

    // The rejection taught the runtime window ("n_ctx is 2048") and the
    // client must surface it even though the call threw: Agent::resolve_window
    // clamps the gauge and the compression budget with it (FIX-032).
    ASSERT_EQ(client.learned_context_size(), 2048);
    close(srv);
}

TEST(overflow_400_buffered_throws_non_retryable_api_error) {
    // OpenAI-style JSON-wrapped overflow rejection on the buffered path.
    std::string req;
    const std::string body = "{\"error\":{\"message\":\"This model's maximum context length is "
                             "16384 tokens. However, you requested 20000 tokens.\"}}";
    int srv = spawn_mock_http(8934, req, "HTTP/1.1 400 Bad Request", "application/json", body);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8934/v1";
    agent::HttpLLMClient client(cfg);
    bool threw = false;
    try {
        client.chat({}, {}, nullptr);
    } catch (const agent::ApiError& e) {
        threw = true;
        ASSERT_EQ(e.status, 400L);
        ASSERT_FALSE(e.retryable);
    }
    ASSERT(threw);
    // "maximum context length is 16384 tokens" — surfaced despite the throw.
    ASSERT_EQ(client.learned_context_size(), 16384);
    close(srv);
}

TEST(client_serves_next_turn_after_overflow_rejection) {
    // The 400 path mutates the client's internal Config (context_size learned,
    // context_explicit set). A later healthy request on the SAME client must
    // be unaffected — the rejection must not poison subsequent turns.
    std::string req;
    int srv400 = spawn_mock_http(8935, req, "HTTP/1.1 400 Bad Request", "text/plain",
                                 "maximum context length is 8192 tokens");
    ASSERT(srv400 >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8935/v1";
    cfg.stream = true;
    agent::HttpLLMClient client(cfg);
    try {
        client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
        ASSERT(false); // the 400 must have thrown
    } catch (const agent::ApiError&) {
    }
    close(srv400);

    // Rebind the same port (SO_REUSEADDR) with a healthy SSE responder and
    // drive the SAME client instance again.
    const std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"still works\"}}]}\n\n"
                            "data: [DONE]\n\n";
    std::string dummy;
    int srv2 = spawn_mock_sse(8935, dummy, sse);
    ASSERT(srv2 >= 0);
    usleep(100000);

    agent::Message m = client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
    ASSERT_EQ(m.content, "still works");
    // The teaching is sticky: the healthy turn neither clears nor changes it.
    ASSERT_EQ(client.learned_context_size(), 8192);
    close(srv2);
}

// The window a 400 overflow rejection teaches is a fact about the request, so
// the client records a notice: the agent drains it into a [harness] message and
// the model learns why its room to move shrank (rather than being clamped
// behind its back).
TEST(overflow_400_records_a_notice_for_the_model) {
    std::string req;
    int srv = spawn_mock_http(8960, req, "HTTP/1.1 400 Bad Request", "text/plain",
                              "maximum context length is 16384 tokens");
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8960/v1";
    agent::HttpLLMClient client(cfg);
    bool threw = false;
    try {
        client.chat({}, {}, nullptr);
    } catch (const agent::ApiError&) {
        threw = true;
    }
    ASSERT(threw);

    auto notices = client.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    ASSERT(notices[0].find("16384") != std::string::npos);
    ASSERT(client.take_notices().empty()); // draining empties the list
    close(srv);
}

// A tool call with unparseable arguments is dropped (malformed calls poison
// later requests), and the drop is reported — silently discarding a call the
// model believes it made is exactly the kind of hidden state that produced the
// endless re-issuing loop.
TEST(malformed_tool_calls_are_reported_and_dropped) {
    std::string dummy;
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
        "\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{not "
        "json\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8961, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8961/v1";
    agent::HttpLLMClient client(cfg);
    agent::Message m = client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
    ASSERT(m.tool_calls.is_null() || m.tool_calls.empty());

    auto notices = client.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    ASSERT(notices[0].find("invalid arguments") != std::string::npos);
    close(srv);
}

TEST(parameter_400_surfaces_its_message_and_teaches_no_window) {
    // A 400 that names a request parameter is not a window statement: nothing
    // may be learned, and the operator must see the server's own sentence
    // ("max_tokens is too large: 16384 ...") rather than a stub of the raw
    // body. Without that, the rejection that causes it is unreadable, which is
    // exactly the bare "HTTP 400 from LLM server" a /compress failure showed.
    std::string req;
    json body;
    body["request_id"] = std::string(300, 'r'); // the message is not the first field
    body["error"] = json{{"message", "max_tokens is too large: 16384. This model supports at most "
                                     "8192 completion tokens, and you requested 16384, which is "
                                     "8192 too many."},
                         {"type", "invalid_request_error"},
                         {"param", "max_tokens"},
                         {"code", nullptr}};
    int srv =
        spawn_mock_http(8951, req, "HTTP/1.1 400 Bad Request", "application/json", body.dump());
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8951/v1";
    cfg.model = "kilo-auto/free";
    cfg.max_tokens = 16384;    // the setting that caused it
    cfg.context_size = 262144; // the stale window it was sent to
    agent::HttpLLMClient client(cfg);
    bool threw = false;
    try {
        agent::Message hello;
        hello.role = "user";
        hello.content = "hi";
        client.chat({hello}, {}, nullptr);
    } catch (const agent::ApiError& e) {
        threw = true;
        ASSERT_EQ(e.status, 400L);
        ASSERT_FALSE(e.retryable);
        ASSERT(std::string(e.what()).find("max_tokens is too large: 16384") != std::string::npos);
    }
    ASSERT(threw);
    // A parameter rejection teaches no window: shrinking one cannot fix it.
    ASSERT_EQ(client.learned_context_size(), 0);
    close(srv);
}

TEST(parameter_400_quoting_a_window_teaches_no_window) {
    // The hybrid shape: an output-parameter rejection that also quotes a
    // window. The numeric sniffer finds "maximum context length is 8192" and
    // would record 8192 — a window the provider never said the prompt outgrew.
    // Lowering max_tokens fixes this request; believing 8192 is a lie the
    // compression gate and the gauge would act on.
    std::string req;
    const std::string body =
        R"({"error":{"code":400,"message":"max_tokens is too large: 16384. This endpoint's )"
        R"(maximum context length is 8192 tokens, and the completion budget counts against it.","type":)"
        R"("invalid_request_error","param":"max_tokens"}})";
    int srv = spawn_mock_http(8952, req, "HTTP/1.1 400 Bad Request", "application/json", body);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8952/v1";
    cfg.max_tokens = 16384;
    agent::HttpLLMClient client(cfg);
    bool threw = false;
    try {
        client.chat({}, {}, nullptr);
    } catch (const agent::ApiError& e) {
        threw = true;
        ASSERT_FALSE(e.retryable);
    }
    ASSERT(threw);
    ASSERT_EQ(client.learned_context_size(), 0);
    close(srv);
}

TEST(llm_cancel_pre_requested_aborts_with_cancelled_error) {
    // A token already requested before the call must abort fast (the /stop
    // "do not start another turn" path), typed CancelledError — never
    // classified as a retryable failure or degraded into a fake reply.
    int srv = spawn_stall_server(8936);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8936/v1";
    cfg.cancel_token.request();
    agent::HttpLLMClient client(cfg);
    bool cancelled = false;
    try {
        client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
    } catch (const agent::CancelledError&) {
        cancelled = true;
    }
    ASSERT(cancelled);
    close(srv);
}

TEST(llm_cancel_mid_stream_aborts_with_cancelled_error) {
    // Esc // /stop during a stalled generation: the in-flight transfer must
    // abort (curl's progress callback polls the shared token) with
    // CancelledError, distinct from ApiError and std::runtime_error.
    int srv = spawn_stall_server(8937);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8937/v1";
    agent::HttpLLMClient client(cfg);
    std::exception_ptr err;
    std::thread caller([&]() {
        try {
            client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
        } catch (...) {
            err = std::current_exception();
        }
    });
    usleep(300000);             // let the request go in-flight
    cfg.cancel_token.request(); // copies share the flag with the client
    caller.join();

    ASSERT(err != nullptr);
    std::string kind = "none";
    try {
        std::rethrow_exception(err);
    } catch (const agent::CancelledError&) {
        kind = "cancelled";
    } catch (const agent::ApiError&) {
        kind = "api_error";
    } catch (const std::exception&) {
        kind = "other";
    }
    ASSERT_EQ(kind, "cancelled");
    close(srv);
}

// Inactivity hands the decision back to the agent: the process keeps running,
// nothing is killed on a timer, and the agent chooses to wait again or stop it.
TEST(bash_inactivity_returns_and_leaves_the_process_alive) {
    agent::JobService jobs;
    agent::CancellationToken token;
    auto tool = agent::make_bash_tool(&jobs, token);

    agent::json a = {{"command", "echo hello-from-bash; sleep 30"}, {"timeout", 1}};
    agent::ToolResult r = tool->execute(a);

    ASSERT(r.ok);
    ASSERT_EQ(r.meta.value("stop", std::string("none")), "inactive");
    ASSERT(r.output.find("still running") != std::string::npos);
    ASSERT(r.output.find("hello-from-bash") != std::string::npos);

    const std::string id = r.meta.value("job", std::string(""));
    ASSERT(!id.empty());
    std::shared_ptr<agent::Job> j = jobs.get(id);
    ASSERT(j != nullptr);
    ASSERT(!j->is_done()); // alive: silence returned control, it did not kill

    jobs.stop(id);
    ASSERT(jobs.get(id) == nullptr); // killed, reaped, erased
}

// ---------------------------------------------------------------------------
// Session persistence
// ---------------------------------------------------------------------------

TEST(session_json_roundtrip_preserves_messages) {
    agent::Session s;
    s.model = "test-model";
    agent::Message sys;
    sys.role = "system";
    sys.content = "be helpful";
    agent::Message u;
    u.role = "user";
    u.content = "hi\nthere";
    agent::Message a;
    a.role = "assistant";
    a.content = "hello";
    a.reasoning = "think first";
    s.messages = {sys, u, a};
    s.derive_title();
    ASSERT_EQ(s.title, "hi");

    agent::json j = s.to_json();
    agent::Session back = agent::Session::from_json(j);
    ASSERT_EQ(back.model, "test-model");
    ASSERT_EQ(back.messages.size(), 3u);
    ASSERT_EQ(back.messages[0].role, "system");
    ASSERT_EQ(back.messages[1].content, "hi\nthere");
    ASSERT_EQ(back.messages[2].reasoning, "think first");
}

// The Anthropic tool loop requires signed thinking blocks back verbatim. A
// session saved between the assistant turn and its tool result must preserve
// them, or the next start replays a turn the API rejects.
TEST(session_json_roundtrip_preserves_protocol_blocks) {
    agent::Session s;
    agent::Message a;
    a.role = "assistant";
    a.content = "on it";
    a.raw_content =
        agent::json::array({{{"type", "thinking"}, {"thinking", "read it"}, {"signature", "sig_1"}},
                            {{"type", "tool_use"},
                             {"id", "toolu_1"},
                             {"name", "read"},
                             {"input", {{"path", "a.txt"}}}}});
    a.tool_calls = agent::json::array(
        {{{"id", "toolu_1"},
          {"type", "function"},
          {"function", {{"name", "read"}, {"arguments", R"({"path":"a.txt"})"}}}}});
    s.messages = {a};

    const agent::Session back = agent::Session::from_json(s.to_json());
    ASSERT_EQ(back.messages.size(), 1u);
    ASSERT(back.messages[0].raw_content.is_array());
    ASSERT_EQ(back.messages[0].raw_content.size(), 2u);
    ASSERT_EQ(back.messages[0].raw_content[0]["signature"], "sig_1");
    ASSERT_EQ(back.messages[0].raw_content[1]["input"]["path"], "a.txt");
}

TEST(session_store_save_load_list_delete) {
    std::string dir = "/tmp/amber_sessions_test";
    run_cmd("rm -rf " + dir);
    agent::SessionStore store(dir);

    agent::Session s1;
    agent::Message u;
    u.role = "user";
    u.content = "first";
    s1.messages = {u};
    s1.derive_title();
    ASSERT_TRUE(store.save(s1));
    ASSERT_FALSE(s1.id.empty());
    ASSERT_TRUE(s1.updated_ms > 0);

    agent::Session loaded;
    ASSERT_TRUE(store.load(s1.id, loaded));
    ASSERT_EQ(loaded.messages.size(), 1u);
    ASSERT_EQ(loaded.messages[0].content, "first");

    usleep(2000);
    agent::Session s2;
    agent::Message u2;
    u2.role = "user";
    u2.content = "second";
    s2.messages = {u2};
    s2.derive_title();
    ASSERT_TRUE(store.save(s2));

    auto metas = store.list();
    ASSERT_EQ(metas.size(), 2u);
    // Newest updated first.
    ASSERT_EQ(metas[0].id, s2.id);
    ASSERT_EQ(metas[0].message_count, 1);

    ASSERT_TRUE(store.remove(s1.id));
    ASSERT_EQ(store.list().size(), 1u);
    agent::Session gone;
    ASSERT_FALSE(store.load(s1.id, gone));
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// bash tool
// ---------------------------------------------------------------------------

TEST(bash_tool_runs_and_reports_exit) {
    auto tool = agent::make_bash_tool();
    ASSERT_TRUE(tool->requires_approval({{"command", "rm -rf /tmp/test"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "echo hello"}}));
    auto ok = tool->execute({{"command", "echo hello"}});
    ASSERT_TRUE(ok.ok);
    ASSERT(ok.output.find("hello") != std::string::npos);
    ASSERT(ok.output.find("[exit 0]") != std::string::npos);

    auto bad = tool->execute({{"command", "exit 3"}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.output.find("[exit 3]") != std::string::npos);
    ASSERT(bad.error.find("status 3") != std::string::npos);
}

TEST(bash_tool_missing_command_errors) {
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"timeout", 5}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("command") != std::string::npos);
}

TEST(bash_tool_runs_in_workspace_root) {
    agent::Workspace::set_root("/tmp/amber_bash_ws");
    run_cmd("rm -rf /tmp/amber_bash_ws && mkdir -p /tmp/amber_bash_ws");
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"command", "pwd"}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("/tmp/amber_bash_ws") != std::string::npos);
}

TEST(bash_tool_times_out) {
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"command", "sleep 5"}, {"timeout", 1}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("timed out") != std::string::npos);
    ASSERT(r.output.find("timed out") != std::string::npos);
}

// Idle timeout (not a fixed wall-clock budget): a command that keeps emitting
// output must survive well past its timeout, while a silent one is killed.
TEST(bash_tool_idle_timeout_keeps_progressing) {
    auto tool = agent::make_bash_tool();
    // ~3s of runtime, output every 0.3s, timeout 1s: never idle long enough.
    auto r =
        tool->execute({{"command", "for i in 1 2 3 4 5 6 7 8 9 10; do echo tick; sleep 0.3; done"},
                       {"timeout", 1}});
    ASSERT(r.ok);
    ASSERT(r.output.find("[exit 0]") != std::string::npos);
    ASSERT(r.output.find("timed out") == std::string::npos);
}

TEST(bash_tool_truncates_large_output) {
    auto tool = agent::make_bash_tool();
    // yes emits far more than the 64 KiB cap; head bounds the runtime.
    auto r = tool->execute({{"command", "yes AAAAAAAAAA | head -c 200000"}, {"timeout", 30}});
    ASSERT(r.output.find("[output truncated") != std::string::npos);
    ASSERT(r.output.size() < static_cast<std::size_t>(70) * 1024u);
}

// When the host wires a JobService, bash spawns through it so the process is
// visible in /job (and on the status bar) while it runs and is killable. The
// synchronous result returned to the model must be identical to the direct
// path, and the job must be cleaned up (erased) once the command finishes.
TEST(bash_tool_tracked_by_job_service) {
    agent::JobService jobs;
    auto tool = agent::make_bash_tool(&jobs);

    auto ok = tool->execute({{"command", "echo tracked"}});
    ASSERT_TRUE(ok.ok);
    ASSERT(ok.output.find("tracked") != std::string::npos);
    ASSERT(ok.output.find("[exit 0]") != std::string::npos);
    ASSERT(jobs.list().empty()); // finished job erased, not leaked

    auto bad = tool->execute({{"command", "exit 7"}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.output.find("[exit 7]") != std::string::npos);
    ASSERT(jobs.list().empty());

    // Silence is not a kill on this path: control returns to the agent, the
    // process stays registered, and ending it is the agent's call.
    auto to = tool->execute({{"command", "sleep 5"}, {"timeout", 1}});
    ASSERT_TRUE(to.ok);
    ASSERT_EQ(to.meta.value("stop", std::string("none")), "inactive");
    const std::string id = to.meta.value("job", std::string(""));
    ASSERT(!id.empty());
    ASSERT_EQ(jobs.list().size(), 1u); // still running, visible in /job
    jobs.stop(id);
    ASSERT(jobs.list().empty()); // stopped and reaped on request
}

// A requested cancellation token must stop the command AND stay requested:
// the token may be shared session state (the /stop flag), so the tool must not
// clear it — otherwise a later cancellation of other in-flight work is lost.
TEST(bash_tool_cancel_token_stays_requested) {
    agent::CancellationToken token;
    agent::JobService jobs;
    auto tool = agent::make_bash_tool(&jobs, token);

    token.request();
    auto r = tool->execute({{"command", "sleep 30"}, {"timeout", 60}});
    ASSERT_FALSE(r.ok);
    ASSERT(token.is_requested()); // must NOT have been cleared
    ASSERT(jobs.list().empty());  // job killed and erased

    // The same token still cancels subsequent work.
    token.clear();
    auto ok = tool->execute({{"command", "echo still-works"}});
    ASSERT_TRUE(ok.ok);
    ASSERT(!token.is_requested());
}

TEST(bash_approval_allowlist) {
    auto tool = agent::make_bash_tool();
    // Read-only commands run without approval.
    ASSERT_FALSE(tool->requires_approval({{"command", "ls -la"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "cat foo.txt"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "git status"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "grep -rn foo ."}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "echo 'rm -rf /'"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "echo \"rm -rf /\""}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "git diff HEAD~1"}}));
    // Mutating / bypass attempts require approval.
    ASSERT_TRUE(tool->requires_approval({{"command", "rm file.txt"}}));
    ASSERT_TRUE(tool->requires_approval({{"command", "/bin/rm -rf /"}})); // qualified path
    ASSERT_TRUE(tool->requires_approval({{"command", "sudo ls"}}));       // prefix command
    ASSERT_TRUE(
        tool->requires_approval({{"command", "git push origin main"}})); // non-read-only subcmd
    ASSERT_TRUE(tool->requires_approval({{"command", "git reset --hard"}}));
    ASSERT_TRUE(tool->requires_approval({{"command", "find . -delete"}}));
    ASSERT_TRUE(tool->requires_approval({{"command", "sed -i s/a/b/ f"}}));
    ASSERT_TRUE(tool->requires_approval({{"command", "echo hi > file.txt"}})); // redirect
    ASSERT_TRUE(tool->requires_approval({{"command", "echo hi >file.txt"}}));  // glued redirect
    ASSERT_TRUE(tool->requires_approval({{"command", "echo hi >>file.txt"}})); // glued append
    ASSERT_TRUE(tool->requires_approval({{"command", "echo $(rm -rf /)"}}));   // cmd substitution
    ASSERT_TRUE(tool->requires_approval({{"command", "cat a && rm b"}}));      // composition
    ASSERT_TRUE(tool->requires_approval({{"command", "cat a | grep b"}}));     // pipe
    ASSERT_TRUE(tool->requires_approval({{"command", "rm -fr /"}}));           // reordered flags
}

// ---------------------------------------------------------------------------
// Approval gate (side-effecting tools require host approval)
// ---------------------------------------------------------------------------

// The core prompt is the agnostic harness: platform, profession and workflow
// belong to the environment card and the domain blocks. This guard exists so a
// later edit cannot quietly re-leak domain content into core — the conflict
// surface the domain seam was built to keep small.
TEST(environment_card_states_the_shell_and_platform_facts) {
    agent::EnvironmentInfo info;
    info.os = "Ubuntu 24.04 (Linux 6.8.0) (WSL)";
    info.workspace = "/w/project";
    info.shell = "/bin/zsh";
    info.tools = {"git", "python3"};
    const std::string card = agent::render_environment_card(info);
    ASSERT(card.find("(WSL)") != std::string::npos);
    ASSERT(card.find("Shell: /bin/zsh") != std::string::npos);
    ASSERT(card.find("Workspace: /w/project") != std::string::npos);
    // Amber's own layout travels with the environment card: a request like
    // "turn on MCP server X" needs the command *and* the place.
    ASSERT(card.find("Amber files: config ") != std::string::npos);
    ASSERT(card.find("/get config paths") != std::string::npos);
}

TEST(tool_envelope_caps_meta_and_error_text) {
    // The fields around a result are what make context expensive: an error dump
    // or a metadata blob arrives whole and is read whole. Both are cut with a
    // marker, so a truncated value is visible as one.
    agent::ToolResult big;
    big.ok = false;
    big.error = std::string(6000, 'x');
    big.meta = {{"blob", std::string(2000, 'y')}};
    const std::string env = agent::format_tool_envelope("bash", {{"command", "boom"}}, big);
    ASSERT(env.find("more characters]") != std::string::npos);
    ASSERT(env.size() < 3000u);
    ASSERT(env.find("status=error") != std::string::npos);

    // A small result is untouched: the caps change nothing a normal call sees.
    agent::ToolResult small;
    small.ok = false;
    small.error = "no such file: a.txt";
    const std::string small_env = agent::format_tool_envelope("read", {{"path", "a.txt"}}, small);
    ASSERT(small_env.find("ERROR: no such file: a.txt") != std::string::npos);
    ASSERT(small_env.find("more characters]") == std::string::npos);
}

static bool tool_budget_utf8_valid(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            return false;
        if (i + len > s.size())
            return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

TEST(tool_envelope_caps_a_large_result_middle_out) {
    // A single long read must not dominate the context: the budget keeps both
    // ends (headers and final errors live there) and states what was omitted.
    agent::ToolResult big;
    big.output = "HEAD\n" + std::string(100 * 1024, 'm') + "\nTAIL";
    const std::size_t budget = 16 * 1024;
    const std::string env = agent::format_tool_envelope("read", {{"path", "big.log"}}, big, budget);
    ASSERT(env.find("HEAD") != std::string::npos);
    ASSERT(env.find("TAIL") != std::string::npos);
    ASSERT(env.find("bytes omitted") != std::string::npos);
    ASSERT(env.size() < budget + 1024u);
    ASSERT(env.size() < big.output.size());
}

TEST(tool_envelope_truncation_is_utf8_safe) {
    // Cutting at a byte offset would split a multi-byte code point and make
    // the envelope invalid UTF-8, which nlohmann rejects on dump.
    std::string body = "HEAD\n";
    for (int i = 0; i < 40000; ++i)
        body += "\xc3\xa9"; // é, two bytes
    body += "\nTAIL";
    agent::ToolResult r;
    r.output = body;
    const std::string env = agent::format_tool_envelope("read", json::object(), r, 4096);
    ASSERT(tool_budget_utf8_valid(env));
    ASSERT(env.find("HEAD") != std::string::npos);
    ASSERT(env.find("TAIL") != std::string::npos);
    ASSERT(env.size() < 8192u);
}

TEST(tool_envelope_reports_original_bytes_and_truncation_in_meta) {
    agent::ToolResult big;
    big.output.assign(64 * 1024, 'z');
    const std::string env = agent::format_tool_envelope("read", json::object(), big, 8 * 1024);
    ASSERT(env.find("\"bytes\":65536") != std::string::npos);
    ASSERT(env.find("\"truncated\":true") != std::string::npos);

    agent::ToolResult small;
    small.output = "ok";
    const std::string small_env =
        agent::format_tool_envelope("read", json::object(), small, 8 * 1024);
    ASSERT(small_env.find("\"bytes\":2") != std::string::npos);
    ASSERT(small_env.find("\"truncated\"") == std::string::npos);
    ASSERT(small_env.find("bytes omitted") == std::string::npos);

    // A zero budget means "no cap" (an explicit opt-out).
    const std::string uncapped =
        agent::format_tool_envelope("read", json::object(), big, std::size_t{0});
    ASSERT(uncapped.find("bytes omitted") == std::string::npos);
    ASSERT(uncapped.find("\"truncated\"") == std::string::npos);
}

TEST(core_system_prompt_stays_agnostic) {
    std::ifstream in("prompts/system.md");
    ASSERT(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    ASSERT(!text.empty());

    for (const char* word : {"Linux", "macOS", "Windows", "engineer", "git ", "commit"})
        ASSERT(text.find(word) == std::string::npos);
    // The identity line names an assistant, not a profession.
    ASSERT(text.find("an AI assistant") != std::string::npos);
    // The load-bearing behaviours are present, so a rewrite cannot hollow it.
    ASSERT(text.find("Information you can obtain, you obtain") != std::string::npos);
    ASSERT(text.find("The work is shared as") != std::string::npos);
    ASSERT(text.find("A progress note is a line or two") != std::string::npos);
    ASSERT(text.find("the word `done`") != std::string::npos);
}

TEST(agent_denies_gated_tool_without_handler) {
    // A registry containing an approval-required tool. With no on_approval
    // handler installed, approve_call must fail safe (deny).
    agent::Config cfg;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::Agent ag(cfg, reg); // hooks default: no on_approval
    // No public approve_call, so exercise via the hook contract instead:
    agent::AgentHooks hooks;
    bool called = false;
    hooks.on_approval = [&](const std::string& n, const agent::json&, const std::string& s) {
        called = true;
        ASSERT_EQ(n, std::string("bash"));
        ASSERT(s.find("run:") != std::string::npos);
        return agent::Approval::AllowSession;
    };
    ag.set_hooks(hooks);
    auto t = reg.find("bash");
    ASSERT_TRUE(t != nullptr);
    ASSERT_TRUE(t->requires_approval({{"command", "rm -rf /tmp/test"}}));
    ASSERT_FALSE(t->requires_approval({{"command", "ls"}}));
    agent::Approval d =
        hooks.on_approval("bash", {{"command", "ls"}}, t->summarize({{"command", "ls"}}));
    ASSERT_TRUE(called);
    ASSERT(d == agent::Approval::AllowSession);
}

// ---------------------------------------------------------------------------
// A denial must say why. The model's only honest report of an unexplained
// refusal is "it was refused", which is exactly what makes an agent stop
// asking and start guessing at the filesystem.
// ---------------------------------------------------------------------------

namespace {

// One gated write_skill call through the real dispatch path; returns what the
// model got back.
agent::ToolResult denied_write_skill_result(const agent::Config& cfg, agent::ToolRegistry& reg,
                                            agent::AgentHooks hooks, agent::PolicyStore* policy) {
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::ToolResult captured;
    captured.ok = true;
    hooks.on_tool_result = [&](const std::string&, const agent::ToolResult& r, const agent::json&) {
        captured = r;
    };
    agent::json calls = agent::json::array();
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {
        {"name", "write_skill"},
        {"arguments", {{"name", "demo"}, {"description", "a demo"}, {"body", "steps"}}}};
    calls.push_back(tc);
    agent::Context ctx;
    agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, policy, nullptr, &ctx);
    return captured;
}

} // namespace

TEST(denied_write_skill_without_an_approver_says_so) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);

    const agent::ToolResult r =
        denied_write_skill_result(cfg, reg, agent::AgentHooks{} /* no approver */, nullptr);
    ASSERT_FALSE(r.ok);
    ASSERT(!r.error.empty());
    ASSERT(r.error.find("approv") != std::string::npos);
}

TEST(denied_tool_without_a_policy_store_or_an_approver_says_so) {
    // The legacy path (no policy store): the tool's own contract asks, and
    // nothing can answer — the reason says which fail-safe fired.
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);

    const agent::ToolResult r =
        denied_write_skill_result(cfg, reg, agent::AgentHooks{}, /*policy=*/nullptr);
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("approv") != std::string::npos);
}

TEST(denied_tool_with_a_policy_store_and_no_approver_says_so) {
    // Same fail-safe through the policy path: a store exists, the verdict is
    // Prompt, and there is still nobody to ask.
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);
    agent::PolicyStore policy;

    const agent::ToolResult r = denied_write_skill_result(cfg, reg, agent::AgentHooks{}, &policy);
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("approv") != std::string::npos);
}

TEST(denied_write_skill_by_a_stored_rule_names_the_scope_and_the_way_out) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);
    agent::PolicyStore policy;
    policy.set_rule("write_skill", agent::PolicyLevel::AlwaysDeny);
    // The rule refuses before anyone is asked: the hook allows, and must not be
    // consulted at all.
    bool asked = false;
    agent::AgentHooks hooks;
    hooks.on_approval = [&](const std::string&, const agent::json&, const std::string&) {
        asked = true;
        return agent::Approval::AllowOnce;
    };

    const agent::ToolResult r = denied_write_skill_result(cfg, reg, hooks, &policy);
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(asked);
    ASSERT(r.error.find("write_skill") != std::string::npos);      // the scope
    ASSERT(r.error.find("/set policy rule") != std::string::npos); // how to change it
}

TEST(denied_write_skill_by_the_user_says_the_user_declined) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);
    agent::AgentHooks hooks;
    hooks.on_approval = [](const std::string&, const agent::json&, const std::string&) {
        return agent::Approval::Deny;
    };

    const agent::ToolResult r = denied_write_skill_result(cfg, reg, hooks, nullptr);
    ASSERT_FALSE(r.ok);
    ASSERT(!r.error.empty());
    ASSERT(r.error.find("user") != std::string::npos);
    ASSERT(r.error.find("declin") != std::string::npos);
}

TEST(denied_write_skill_by_always_deny_names_the_rule) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);
    // With a policy store the choice persists, so the reason names the rule and
    // the way to take it back.
    agent::PolicyStore policy;
    agent::AgentHooks hooks;
    hooks.on_approval = [](const std::string&, const agent::json&, const std::string&) {
        return agent::Approval::AlwaysDeny;
    };

    const agent::ToolResult r = denied_write_skill_result(cfg, reg, hooks, &policy);
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("always") != std::string::npos);
    ASSERT(r.error.find("/set policy rule") != std::string::npos);
    const agent::PolicyRule* rule = policy.find("write_skill");
    ASSERT(rule != nullptr);
    ASSERT(rule->level == agent::PolicyLevel::AlwaysDeny);
}

// ---------------------------------------------------------------------------
// Shell command classification (read-only vs benign-write vs destructive vs
// outside-workspace). The classifier drives the approval gate: read-only and
// benign in-workspace writes run free in WRITE mode; destructive commands and
// outside-workspace paths prompt.
// ---------------------------------------------------------------------------

TEST(shell_classify_readonly) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // Read-only commands never require approval.
    ASSERT(agent::classify_shell("ls -la", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("wc -l file.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("git status", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("grep -rn foo .", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("cat < input.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    agent::Workspace::set_root(".");
}

TEST(shell_classify_write_vs_destructive) {
    // Benign in-workspace writes run free in WRITE mode.
    ASSERT(agent::classify_shell("echo x > out.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Write);
    ASSERT(agent::classify_shell("mv a.txt b.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Write);
    ASSERT(agent::classify_shell("touch newfile", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Write);
    // Destructive commands prompt.
    ASSERT(agent::classify_shell("rm -rf build", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Destructive);
    ASSERT(agent::classify_shell("git reset --hard", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Destructive);
    // Composition/redirection ends the read-only claim.
    ASSERT(agent::classify_shell("echo hi > file.txt", "/tmp/amber_cls_ws").effect !=
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("cat a && rm b", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Destructive);
    ASSERT(agent::classify_shell("cat a | grep b", "/tmp/amber_cls_ws").effect !=
           agent::ShellEffect::ReadOnly);
}

TEST(shell_classify_outside_workspace) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // A path escaping the sandbox is Outside with a folder scope id.
    auto c1 = agent::classify_shell("echo x > /tmp/amber_outside_ws/f", "/tmp/amber_cls_ws");
    ASSERT(c1.effect == agent::ShellEffect::Outside);
    ASSERT(c1.scope_id == "outside:/tmp/amber_outside_ws");
    auto c2 = agent::classify_shell("rm /etc/hosts", "/tmp/amber_cls_ws");
    ASSERT(c2.effect == agent::ShellEffect::Outside);
    ASSERT(c2.scope_id == "outside:/etc");
    agent::Workspace::set_root(".");
}

TEST(shell_requires_approval_matches_classifier) {
    // The bash tool's coarse approval contract delegates to the classifier.
    auto tool = agent::make_bash_tool();
    ASSERT_FALSE(tool->requires_approval({{"command", "ls"}}));
    ASSERT_TRUE(tool->requires_approval({{"command", "rm -rf x"}}));
    ASSERT_FALSE(tool->requires_approval({{"command", "git status"}}));
}

// ---------------------------------------------------------------------------
// SEC-01: shell classifier security bypasses (Red tests).
// Reader heads must not exempt out-of-workspace path arguments from
// confinement; newline must act as a chain operator; input-redirect targets
// must be confined; `cd` is not a read-only command.
// ---------------------------------------------------------------------------

TEST(sec01_reader_args_outside_workspace) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // Reader heads with outside-workspace args must be Outside, not ReadOnly.
    ASSERT(agent::classify_shell("cat /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("grep foo /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("ls /tmp", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("head /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("tail /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    // In-workspace reader args stay ReadOnly (common case).
    ASSERT(agent::classify_shell("cat file.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("grep -rn foo .", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    agent::Workspace::set_root(".");
}

TEST(sec01_cd_not_reader) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // `cd` changes process state and enables relative-path escapes; it is
    // never read-only.
    ASSERT(agent::classify_shell("cd /tmp", "/tmp/amber_cls_ws").effect !=
           agent::ShellEffect::ReadOnly);
    // `cd /tmp && cat secret` escapes: the classifier must see the outside
    // path, not treat the whole line as a read-only `cd`.
    ASSERT(agent::classify_shell("cd /tmp && cat secret", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    agent::Workspace::set_root(".");
}

TEST(sec01_newline_is_chain_operator) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // An embedded newline separates commands exactly like ";". The second
    // command reads /etc/passwd, so the whole line must be Outside, not
    // ReadOnly (which the old tokenizer produced by merging both lines into
    // one segment attributed to `echo`).
    ASSERT(agent::classify_shell("echo a\ncat /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    // A newline-only composition of in-workspace readers stays non-ReadOnly
    // (composed commands are never provably read-only), but must not be
    // Outside either.
    auto c = agent::classify_shell("ls .\ncat file.txt", "/tmp/amber_cls_ws");
    ASSERT(c.effect != agent::ShellEffect::ReadOnly);
    ASSERT(c.effect != agent::ShellEffect::Outside);
    agent::Workspace::set_root(".");
}

TEST(sec01_input_redirect_outside) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // Input redirect target must be confined: `cat < /etc/passwd` reads
    // outside the workspace, so it must be Outside, not ReadOnly.
    ASSERT(agent::classify_shell("cat < /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    // In-workspace input redirect stays ReadOnly.
    ASSERT(agent::classify_shell("cat < input.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    agent::Workspace::set_root(".");
}

// ---------------------------------------------------------------------------
// Characterisation tables for the classifier. It drives the approval gate, so
// the split into helpers must not move a single verdict: every row below was
// recorded from the behaviour *before* the split and is asserted unchanged
// after it. Rows are grouped by what they exercise.
// ---------------------------------------------------------------------------

struct ShellRow {
    const char* command;
    agent::ShellEffect effect;
    const char* scope;
};

static const char* shell_effect_name(agent::ShellEffect e) {
    switch (e) {
    case agent::ShellEffect::ReadOnly:
        return "ReadOnly";
    case agent::ShellEffect::Write:
        return "Write";
    case agent::ShellEffect::Destructive:
        return "Destructive";
    case agent::ShellEffect::Outside:
        return "Outside";
    }
    return "?";
}

// Reports the command on failure, so a broken row names the case it broke.
static void check_shell_class(const ShellRow& row) {
    agent::ShellClass got = agent::classify_shell(row.command, "/tmp/amber_cls_ws");
    if (got.effect == row.effect && got.scope_id == row.scope)
        return;
    std::ostringstream os;
    os << "classify_shell(\"" << row.command << "\") = " << shell_effect_name(got.effect) << " '"
       << got.scope_id << "', expected " << shell_effect_name(row.effect) << " '" << row.scope
       << "'";
    agent::test::fail(os.str());
}

static void check_shell_rows(const ShellRow* rows, std::size_t n) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    for (std::size_t i = 0; i < n; ++i)
        check_shell_class(rows[i]);
    agent::Workspace::set_root(".");
}

// Reader heads, head shape, and git read-only subcommands. A head that is
// qualified (contains '/'), flag-like or empty cannot be proven safe.
TEST(shell_classify_table_heads_and_git) {
    const ShellRow rows[] = {
        {"ls", agent::ShellEffect::ReadOnly, ""},
        {"ls -la", agent::ShellEffect::ReadOnly, ""},
        {"wc -l file.txt", agent::ShellEffect::ReadOnly, ""},
        {"pwd", agent::ShellEffect::ReadOnly, ""},
        {"echo hi", agent::ShellEffect::ReadOnly, ""},
        {"printf x", agent::ShellEffect::ReadOnly, ""},
        {"export A=1", agent::ShellEffect::ReadOnly, ""},
        {"cat file.txt", agent::ShellEffect::ReadOnly, ""},
        {"ls .", agent::ShellEffect::ReadOnly, ""},
        {"VAR=1 ls", agent::ShellEffect::ReadOnly, ""},
        {"VAR=1 cat f", agent::ShellEffect::ReadOnly, ""},
        {"./x.sh", agent::ShellEffect::Destructive, "bash:*"},
        {"/bin/ls", agent::ShellEffect::Destructive, "bash:*"},
        {"-x", agent::ShellEffect::Destructive, "bash:*"},
        {"git status", agent::ShellEffect::ReadOnly, ""},
        {"git log --oneline", agent::ShellEffect::ReadOnly, ""},
        {"git rev-parse HEAD", agent::ShellEffect::ReadOnly, ""},
        {"git help", agent::ShellEffect::ReadOnly, ""},
        {"git add .", agent::ShellEffect::Write, "bash:git add"},
        {"git commit -m x", agent::ShellEffect::Write, "bash:git commit"},
        {"git", agent::ShellEffect::Write, "bash:git"},
        {"git push", agent::ShellEffect::Destructive, "bash:git push"},
        {"git push origin main", agent::ShellEffect::Destructive, "bash:git push"},
        {"git push --force", agent::ShellEffect::Destructive, "bash:git push --force"},
        {"git reset --hard", agent::ShellEffect::Destructive, "bash:git reset"},
        {"git clean -fd", agent::ShellEffect::Destructive, "bash:git clean"},
    };
    check_shell_rows(rows, sizeof(rows) / sizeof(rows[0]));
}

// Redirects, fd dups, escapes, chain operators and the newline separator.
// NOTE: `ls 2>&1` is a known false positive — an fd dup writes no file, but the
// redirect scan treats "&1" as its target and calls the segment a write. It is
// conservative (it prompts more, never less), and this refactor preserves it
// deliberately rather than smuggling a behaviour change into a decomposition.
TEST(shell_classify_table_redirects_and_escapes) {
    const ShellRow rows[] = {
        {"echo x > out.txt", agent::ShellEffect::Write, "bash:echo"},
        {"echo x >> out.txt", agent::ShellEffect::Write, "bash:echo"},
        {"echo hi >file.txt", agent::ShellEffect::Write, "bash:echo"},
        {"echo x 2> err.txt", agent::ShellEffect::Write, "bash:echo"},
        {"echo x >", agent::ShellEffect::Write, "bash:echo"},
        {"ls 2>&1", agent::ShellEffect::Write, "bash:ls"},
        {"echo x > /etc/out", agent::ShellEffect::Outside, "outside:/etc"},
        {"cat < input.txt", agent::ShellEffect::ReadOnly, ""},
        {"cat < /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"ls &", agent::ShellEffect::Destructive, "bash:*"},
        {"ls $(pwd)", agent::ShellEffect::Destructive, "bash:*"},
        {"ls `pwd`", agent::ShellEffect::Destructive, "bash:*"},
        {"ls;cat x", agent::ShellEffect::Destructive, "bash:*"},
        {"ls;", agent::ShellEffect::Destructive, "bash:*"},
        {"echo a|b", agent::ShellEffect::Destructive, "bash:*"},
        {"echo 'a|b'", agent::ShellEffect::ReadOnly, ""},
        {"echo \"a|b\"", agent::ShellEffect::ReadOnly, ""},
        {"ls && cat x", agent::ShellEffect::Write, "bash:*"},
        {"cat a | grep b", agent::ShellEffect::Write, "bash:*"},
        {"echo a\ncat /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"ls .\ncat file.txt", agent::ShellEffect::Write, "bash:*"},
    };
    check_shell_rows(rows, sizeof(rows) / sizeof(rows[0]));
}

// Mutating heads (destructive patterns, find/sed flags, plain writers) and
// workspace-escaping paths.
TEST(shell_classify_table_mutators_and_paths) {
    const ShellRow rows[] = {
        {"rm -rf build", agent::ShellEffect::Destructive, "bash:rm"},
        {"rm build", agent::ShellEffect::Destructive, "bash:rm"},
        {"rmdir d", agent::ShellEffect::Destructive, "bash:rmdir"},
        {"sudo ls", agent::ShellEffect::Destructive, "bash:sudo"},
        {"docker ps", agent::ShellEffect::Destructive, "bash:docker"},
        {"chmod -R 777 x", agent::ShellEffect::Destructive, "bash:chmod -R"},
        {"find . -delete", agent::ShellEffect::Write, "bash:find"},
        {"find . -name x", agent::ShellEffect::Write, "bash:find"},
        {"find . -exec rm {} ;", agent::ShellEffect::Write, "bash:find"},
        {"sed -i s/a/b/ f", agent::ShellEffect::Write, "bash:sed"},
        {"sed 's/a/b/' f", agent::ShellEffect::Write, "bash:sed"},
        {"cp a b", agent::ShellEffect::Write, "bash:cp"},
        {"mv a b", agent::ShellEffect::Write, "bash:mv"},
        {"touch f", agent::ShellEffect::Write, "bash:touch"},
        {"mkdir d", agent::ShellEffect::Write, "bash:mkdir"},
        {"cat /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"grep foo /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"ls /tmp", agent::ShellEffect::Outside, "outside:/"},
        {"head /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"wc -l /etc/passwd", agent::ShellEffect::Outside, "outside:/etc"},
        {"cd /tmp", agent::ShellEffect::Outside, "outside:/"},
        {"cd /tmp && cat secret", agent::ShellEffect::Outside, "outside:/"},
    };
    check_shell_rows(rows, sizeof(rows) / sizeof(rows[0]));
}

// ---------------------------------------------------------------------------
// SEC-02: approval-gate coverage for side-effecting tools (Red tests).
// WriteTool (create/overwrite), TaskTool (sub-agent privilege escalation),
// and PluginTool (external-process code execution) must opt into the
// approval gate via requires_approval.
// ---------------------------------------------------------------------------

TEST(sec02_write_tool_create_requires_approval) {
    auto tool = agent::make_write_tool();
    // Create/overwrite (old == "") must require approval.
    json create = {{"path", "x"}, {"edits", json::array({{{"old", ""}, {"new", "y"}}})}};
    ASSERT_TRUE(tool->requires_approval(create));
}

TEST(sec02_write_tool_patch_no_approval) {
    auto tool = agent::make_write_tool();
    // In-place patch (old != "") does not require approval — the common
    // agent workflow of editing existing files should not be gated.
    json patch = {{"path", "x"}, {"edits", json::array({{{"old", "a"}, {"new", "b"}}})}};
    ASSERT_FALSE(tool->requires_approval(patch));
}

TEST(sec02_task_tool_requires_approval) {
    agent::SubAgentExecutor executor;
    agent::ToolRegistry reg;
    auto tool = agent::make_task_tool(executor, reg);
    // TaskTool spawns a sub-agent with full tool access — always prompt.
    ASSERT_TRUE(tool->requires_approval({{"prompt", "anything"}}));
    ASSERT_TRUE(tool->requires_approval({}));
}

TEST(sec02_plugin_tool_requires_approval) {
    // Stage the fake plugin and enable it to get a PluginTool into the
    // registry, then verify it requires approval.
    std::string base = "/tmp/amber_sec02_plugin";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    // EnvGuard equivalent
    const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string saved_xdg = old_xdg ? old_xdg : "";
    bool was_set = old_xdg != nullptr;
    setenv("XDG_CONFIG_HOME", (base + "/xdg").c_str(), 1);

    std::string dir = base + "/plugins/fake";
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file("tests/plugins/fake_manifest.json", dir + "/manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("tests/plugins/fake_plugin.py", dir + "/fake_plugin.py",
                               std::filesystem::copy_options::overwrite_existing);
    chmod((dir + "/fake_plugin.py").c_str(), 0755);

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    ASSERT(mgr.enable("fake", reg));
    auto echo = reg.find("plugin_fake_echo");
    ASSERT(echo != nullptr);
    // PluginTool runs external-process code — always prompt.
    ASSERT_TRUE(echo->requires_approval({{"text", "hello"}}));
    ASSERT_TRUE(echo->requires_approval({}));

    if (was_set)
        setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
    else
        unsetenv("XDG_CONFIG_HOME");
}

// ---------------------------------------------------------------------------
// Approval gate decision engine (mode x classification x stored rules)
// ---------------------------------------------------------------------------

TEST(policy_engine_modes) {
    agent::Config cfg;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();
    const json args = {{"command", "rm -rf x"}};

    // Read mode: destructive calls deny silently, no prompt.
    cfg.mode = agent::AgentMode::Read;
    agent::Decision d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::DenySilent);

    // Write mode: destructive calls prompt.
    cfg.mode = agent::AgentMode::Write;
    d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::Prompt);
    ASSERT(d.scope_id == "bash:rm");

    // Yolo mode: everything runs, no prompt.
    cfg.mode = agent::AgentMode::Yolo;
    d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::Allow);

    // Master switch off: no prompts in write mode.
    cfg.mode = agent::AgentMode::Write;
    cfg.policy_approval = false;
    d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::Allow);
    cfg.policy_approval = true;
}

TEST(policy_always_allow_suppresses_dialog) {
    // THE regression test for the reported bug: an "always allow" rule must
    // suppress the approval dialog for the same command kind, not just persist
    // to JSON and re-prompt.
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();
    const json args = {{"command", "rm -rf x"}};

    policy.set_rule("bash:rm", agent::PolicyLevel::AlwaysAllow);
    agent::Decision d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::Allow);
}

TEST(policy_always_deny_blocks_silently) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();
    const json args = {{"command", "rm -rf x"}};

    policy.set_rule("bash:rm", agent::PolicyLevel::AlwaysDeny);
    agent::Decision d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::DenySilent);
}

TEST(policy_session_grant_per_scope) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();

    // A session grant for rm suppresses a second rm...
    policy.grant_session("bash:rm");
    agent::Decision d1 = agent::decide_approval(cfg, *bash, {{"command", "rm -rf x"}}, policy);
    ASSERT(d1.v == agent::Verdict::Allow);
    // ...but a different destructive command still prompts.
    agent::Decision d2 =
        agent::decide_approval(cfg, *bash, {{"command", "dd if=/dev/zero of=x"}}, policy);
    ASSERT(d2.v == agent::Verdict::Prompt);
    // Clearing session grants restores prompting.
    policy.clear_session();
    agent::Decision d3 = agent::decide_approval(cfg, *bash, {{"command", "rm -rf x"}}, policy);
    ASSERT(d3.v == agent::Verdict::Prompt);
}

TEST(policy_benign_write_free_in_write_mode) {
    // In WRITE mode, benign in-workspace writes must NOT prompt (this is the
    // "ls/wc keep prompting" class of bug, on the write side).
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();
    agent::Workspace::set_root("/tmp/amber_cls_ws");

    agent::Decision d =
        agent::decide_approval(cfg, *bash, {{"command", "echo x > out.txt"}}, policy);
    ASSERT(d.v == agent::Verdict::Allow);
    agent::Workspace::set_root(".");
}

TEST(policy_outside_scope_prompts_and_grants) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Write;
    agent::PolicyStore policy;
    auto bash = agent::make_bash_tool();
    agent::Workspace::set_root("/tmp/amber_cls_ws");

    const json args = {{"command", "echo x > /tmp/amber_outside_ws/f"}};
    agent::Decision d = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d.v == agent::Verdict::Prompt);
    ASSERT(d.scope_id == "outside:/tmp/amber_outside_ws");

    // Granting the folder scope makes subsequent writes to the SAME folder
    // silent; a different outside folder still prompts.
    policy.grant_session("outside:/tmp/amber_outside_ws");
    agent::Decision d2 = agent::decide_approval(cfg, *bash, args, policy);
    ASSERT(d2.v == agent::Verdict::Allow);
    agent::Decision d3 =
        agent::decide_approval(cfg, *bash, {{"command", "echo x > /etc/hosts"}}, policy);
    ASSERT(d3.v == agent::Verdict::Prompt);
    agent::Workspace::set_root(".");
}

TEST(policy_rule_roundtrip_preserves_scopes) {
    // Pattern rules (bash:rm, outside:...) survive a save/load roundtrip.
    agent::PolicyStore policy;
    policy.set_rule("bash:rm", agent::PolicyLevel::AlwaysAllow);
    policy.set_rule("bash:git reset", agent::PolicyLevel::AlwaysDeny);
    policy.set_rule("outside:/tmp/amber_outside_ws", agent::PolicyLevel::AlwaysAllow);
    std::string path = "/tmp/amber_policy_roundtrip.json";
    policy.save(path);

    agent::PolicyStore loaded;
    loaded.load(path);
    const auto* r = loaded.find("bash:rm");
    ASSERT(r != nullptr);
    ASSERT(r->level == agent::PolicyLevel::AlwaysAllow);
    ASSERT(loaded.find("bash:git reset") != nullptr);
    ASSERT(loaded.find("bash:git reset")->level == agent::PolicyLevel::AlwaysDeny);
    ASSERT(loaded.find("outside:/tmp/amber_outside_ws") != nullptr);
    ASSERT(loaded.find("outside:/tmp/amber_outside_ws")->level == agent::PolicyLevel::AlwaysAllow);
    run_cmd("rm -f " + path);
}

// A model that keeps emitting a tool call with EMPTY arguments (e.g. search {})
// triggers a deterministic, non-transient failure. The agent must not loop
// through all max_tool_iterations (which stalls the UI for minutes on a
// single-GPU endpoint) — it should stop after a few identical failures.
TEST(agent_stops_on_repeated_empty_arg_tool_call) {
    // A mock SSE server that re-serves the same "search {}" tool call on every
    // connection (unlike spawn_mock_sse, which accepts only once).
    std::string sse = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
                      "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
                      "\"arguments\":\"{}\"}}]}}]}\n\n"
                      "data: [DONE]\n\n";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8920);
    ASSERT(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0); // NOLINT
    ASSERT(listen(fd, 8) == 0);
    std::thread srv([fd, sse]() {
        while (true) {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0)
                break;
            char buf[4096];
            std::string req;
            while (true) {
                int n = recv(c, buf, sizeof(buf) - 1, 0);
                if (n <= 0)
                    break;
                req.append(buf, n);
                if (req.find("\r\n\r\n") != std::string::npos)
                    break;
            }
            std::string http = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                               "Content-Length: " +
                               std::to_string(sse.size()) + "\r\n\r\n" + sse;
            send(c, http.c_str(), http.size(), 0);
            usleep(100000);
            close(c);
        }
    });
    srv.detach();
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8920/v1";
    cfg.stream = true;
    cfg.mode = agent::AgentMode::Yolo;
    cfg.max_tool_iterations = 32; // default; the loop must NOT reach this
    cfg.detection_loop = true;    // enable loop detection for this test
    cfg.system_prompt_path = "prompts/system.md";
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos);
    agent::Agent ag(cfg, reg);

    // Count tool-call dispatches by watching the on_tool_result hook.
    int tool_results = 0;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string&, const agent::ToolResult&, const agent::json&) {
        ++tool_results;
    };
    ag.set_hooks(hooks);

    std::string out = ag.run("review ncurses usage");
    // Must terminate well before max_tool_iterations (bounded by loop detector).
    ASSERT(tool_results < 10);
    ASSERT(out.find("loop detected") != std::string::npos);
    close(fd);
}

// ---------------------------------------------------------------------------
// Background jobs (JobService + process_* tools)
// ---------------------------------------------------------------------------

TEST(job_service_start_read_stop) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    run_cmd("mkdir -p /tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("printf 'hello\\nworld\\n'", "/tmp/amber_job_ws");
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(jobs.running_count(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // First delta returns the captured output; a second read returns nothing.
    std::string delta = jobs.read_delta(id);
    ASSERT(delta.find("hello") != std::string::npos);
    ASSERT(jobs.read_delta(id).empty());
    // Full output is also retrievable.
    ASSERT(jobs.output(id).find("world") != std::string::npos);
    ASSERT(jobs.stop(id));
    ASSERT_EQ(jobs.running_count(), 0);
    ASSERT_FALSE(jobs.stop(id)); // already gone
}

// A stop asks before it kills: SIGTERM gives the child the chance to flush what
// it holds (a half-written file, a lock, a report line), and SIGKILL is only for
// the ones that will not go. Before this, every stop was a SIGKILL — the child
// never got to say what it was doing.

TEST(job_info_carries_owner_and_label) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    run_cmd("mkdir -p /tmp/amber_job_ws");
    agent::JobService jobs;
    const std::string id = jobs.start("sleep 5", "/tmp/amber_job_ws", 60, 30,
                                      agent::JobOrigin{"window-3", "the label"});
    ASSERT_FALSE(id.empty());
    auto job = jobs.get(id);
    ASSERT(job != nullptr);
    const agent::JobInfo info = job->info();
    ASSERT_EQ(info.owner_id, std::string("window-3"));
    ASSERT_EQ(info.label, std::string("the label"));
    ASSERT(jobs.stop(id));
}

TEST(job_service_stop_asks_before_it_kills) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    run_cmd("mkdir -p /tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("trap 'echo got-term; exit 0' TERM; echo waiting; sleep 300",
                                "/tmp/amber_job_ws");
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto job = jobs.get(id);
    ASSERT(job != nullptr);

    ASSERT(jobs.stop(id));
    ASSERT(job->output().find("got-term") != std::string::npos); // it heard the SIGTERM
    ASSERT_EQ(job->exit_code(), 0);                              // and closed on its own terms
}

// A child that ignores SIGTERM is escalated after the grace, and the stop still
// returns: a group holding its pipe open would otherwise hang the reader join
// forever.
TEST(job_service_stop_escalates_a_sigterm_ignoring_child) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    run_cmd("mkdir -p /tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("trap '' TERM; echo ignoring; sleep 300", "/tmp/amber_job_ws");
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto job = jobs.get(id);
    ASSERT(job != nullptr);
    const int pid = job->info().pid;
    ASSERT(pid > 0);

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT(jobs.stop(id)); // SIGTERM, 2s grace, SIGKILL — and it returns
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT(ms < 5000);
    ASSERT_EQ(jobs.running_count(), 0);
    // The group is gone, not merely forgotten.
    ASSERT(kill(pid, 0) != 0 && errno == ESRCH);
}

TEST(job_service_idle_timeout_kills) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    agent::JobService jobs;
    // No output for 1s -> auto-killed by check_timeouts.
    std::string id = jobs.start("sleep 5", "/tmp/amber_job_ws", 600, 1);
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    jobs.check_timeouts();
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(job_service_hard_timeout_kills) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("sleep 5", "/tmp/amber_job_ws", 1, 600);
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    jobs.check_timeouts();
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(process_tools_share_service) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    ASSERT_EQ(tools.size(), 3u);
    // Drive the tools through one background cycle.
    auto* st = tools[0].get();
    auto r = st->execute({{"command", "printf 'bg\\n'"}});
    ASSERT(r.ok);
    std::string id = r.output;
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto* rd = tools[1].get();
    auto out = rd->execute({{"id", id}, {"all", true}});
    ASSERT(out.ok);
    ASSERT(out.output.find("bg") != std::string::npos);
    auto* stop = tools[2].get();
    auto killed = stop->execute({{"id", id}});
    ASSERT(killed.ok);
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(process_stop_returns_captured_output) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    std::string id = tools[0]->execute({{"command", "printf 'held\\n'"}}).output;
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto stopped = tools[2]->execute({{"id", id}});
    ASSERT(stopped.ok);
    ASSERT(stopped.output.find("held") != std::string::npos);
    ASSERT_EQ(jobs.running_count(), 0);
}

// The wait budget is silence, not a deadline: it returns after the silence it
// asked for, kills nothing, and leaves the job for the agent to read or stop.
TEST(process_read_wait_is_inactivity_and_kills_nothing) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    std::string id = tools[0]->execute({{"command", "echo hi; sleep 30"}}).output;
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto began = std::chrono::steady_clock::now();
    auto read = tools[1]->execute({{"id", id}, {"wait", 1}});
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - began)
                            .count();

    ASSERT(read.ok);
    ASSERT(waited >= 900);              // it honoured the silence it was given
    ASSERT(waited < 5000);              // and nothing kept it there longer
    ASSERT_EQ(jobs.running_count(), 1); // nothing was killed
    tools[2]->execute({{"id", id}});
    ASSERT_EQ(jobs.running_count(), 0); // stop is the agent's call, and it works
}

TEST(job_service_stop_finished_returns_true) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("true", "/tmp/amber_job_ws");
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT(jobs.stop(id));       // still in the map -> true
    ASSERT_FALSE(jobs.stop(id)); // already removed -> false
}

TEST(job_service_caps_output_at_one_mib) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    agent::JobService jobs;
    // Emit ~2 MiB of 'A's; the reader must cap at 1 MiB and flag truncation.
    std::string id = jobs.start("head -c 2000000 /dev/zero | tr '\\0' 'A'", "/tmp/amber_job_ws");
    ASSERT_FALSE(id.empty());
    // Wait (polling) for the reader to hit the cap — a fixed sleep is racy on
    // slow CI runners (macOS), where 2 MiB through the pipe can exceed it.
    std::shared_ptr<agent::Job> j = jobs.get(id);
    ASSERT(j != nullptr);
    bool saw_truncated = false;
    for (int i = 0; i < 50; ++i) {
        agent::JobInfo info = j->info();
        if (info.truncated) {
            saw_truncated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT(saw_truncated);
    agent::JobInfo info = j->info();
    ASSERT(info.bytes <= (1u << 20) + 16);
    jobs.stop(id);
}

// CC-16: a digest is an index, not a deletion. Anything the compactor replaces
// must be recoverable byte-for-byte through the id the digest carries.
static std::string result_id_from_text(const std::string& digest) {
    const std::string key = "result_id=";
    const std::size_t at = digest.find(key);
    if (at == std::string::npos)
        return {};
    const std::size_t from = at + key.size();
    const std::size_t end = digest.find_first_not_of("0123456789abcdef", from);
    return digest.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

// ---------------------------------------------------------------------------
// Dispatch / tool approval tests
// ---------------------------------------------------------------------------

TEST(dispatch_approves_and_runs_valid_tool_call) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;

    int tool_results = 0;
    agent::ToolResult captured;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string& /*n*/, const agent::ToolResult& r,
                               const agent::json&) {
        ++tool_results;
        captured = r;
    };

    agent::json calls = agent::json::array();
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "bash"}, {"arguments", {{"command", "echo hello"}}}};
    calls.push_back(tc);

    agent::Context dctx;
    bool ok =
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx);
    ASSERT(ok);
    ASSERT(tool_results == 1);
    ASSERT(captured.ok);
    ASSERT(!captured.output.empty());
    ASSERT(captured.output.find("hello") != std::string::npos);
    // Tool result must be recorded in context
    bool found = false;
    for (const auto& m : dctx.get_all())
        if (m.role == "tool" && m.name == "bash") {
            found = true;
            break;
        }
    ASSERT(found);
}

TEST(dispatch_projects_bulky_bash_output_at_the_source) {
    // A build log is the one tool result where the middle is noise: keep the
    // ends, hand the model a handle, and leave read untouched.
    XdgGuard xdg("dispatch_bash_projection");
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::AgentHooks hooks;
    agent::json calls = agent::json::array();
    calls.push_back({{"id", "c1"},
                     {"type", "function"},
                     {"function", {{"name", "bash"}, {"arguments", {{"command", "seq 1 4000"}}}}}});
    agent::Context dctx;
    ASSERT(
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx));
    const auto msgs = dctx.get_all();
    const agent::Message& tool = msgs.back();
    ASSERT_EQ(tool.role, "tool");
    ASSERT(tool.content.size() < 10u * 1024);
    const std::string id = result_id_from_text(tool.content);
    ASSERT_EQ(id.size(), 16u);
    std::string error;
    const auto full = agent::read_tool_artifact(id, 0, 65536, "", error);
    ASSERT(full.has_value());
    ASSERT(full->size() > 12u * 1024);
}

TEST(dispatch_rejects_duplicate_tool_call) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    cfg.detection_duplicate = true;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::Context dctx;

    // Pre-populate context with an assistant message that already made this call.
    // This simulates a model repeating a tool call from a prior turn.
    agent::json prior_tc = agent::json::array();
    agent::json tc1;
    tc1["id"] = "prev";
    tc1["type"] = "function";
    tc1["function"] = {{"name", "bash"}, {"arguments", R"({"command":"echo hello"})"}};
    prior_tc.push_back(tc1);
    agent::Message prior;
    prior.role = "assistant";
    prior.content = "";
    prior.tool_calls = prior_tc;
    dctx.push(std::move(prior));

    int tool_results = 0;
    std::vector<agent::ToolResult> results;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string&, const agent::ToolResult& r, const agent::json&) {
        ++tool_results;
        results.push_back(r);
    };

    agent::json calls = agent::json::array();
    // Same tool call — should be rejected as duplicate of 'prior'
    agent::json tc2;
    tc2["id"] = "c1";
    tc2["type"] = "function";
    tc2["function"] = {{"name", "bash"}, {"arguments", {{"command", "echo hello"}}}};
    calls.push_back(tc2);

    bool ok =
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx);
    // Should be rejected as duplicate
    ASSERT_FALSE(ok);
    ASSERT(tool_results == 1);
    ASSERT_FALSE(results[0].ok);
    ASSERT(results[0].error.find("already ran") != std::string::npos);
}

// The duplicate-detection rules, exercised through dispatch because
// find_duplicate_call is internal. A no-op tool keeps it hermetic: the check
// runs before execution, so a rejection and a pass are both observable without
// spawning a shell.
TEST(dispatch_duplicate_detection_rules) {
    struct NoopTool : agent::Tool {
        std::string name() const noexcept override { return "probe"; }
        std::string description() const noexcept override { return "probe"; }
        agent::json parameters_schema() const override { return agent::json::object(); }
        agent::ToolResult execute(const agent::json&) const override { return {true, "ok", ""}; }
    };

    // Runs one scenario: a prior assistant call with `prior_args` and an
    // optional tool result of `prior_outcome`, then the same call again with
    // `current_args`. Returns true when the repeat was rejected.
    auto rejected = [](const agent::json& prior_args, const char* prior_outcome, bool detection,
                       const agent::json& current_args) {
        agent::Config cfg; // Write mode, no policy store
        cfg.detection_duplicate = detection;
        agent::ToolRegistry reg;
        reg.register_tool(std::make_unique<NoopTool>());
        agent::ConversationLog log;
        std::set<std::string> approved;
        agent::Context dctx;

        agent::json prior_calls = agent::json::array();
        prior_calls.push_back({{"id", "prev"},
                               {"type", "function"},
                               {"function", {{"name", "probe"}, {"arguments", prior_args}}}});
        agent::Message prior;
        prior.role = "assistant";
        prior.tool_calls = prior_calls;
        dctx.push(std::move(prior));
        if (prior_outcome) {
            agent::Message out;
            out.role = "tool";
            out.tool_call_id = "prev";
            out.name = "probe";
            out.content = std::string("[tool=probe status=") + prior_outcome + "]\n";
            dctx.push(std::move(out));
        }

        agent::json calls = agent::json::array();
        calls.push_back({{"id", "now"},
                         {"type", "function"},
                         {"function", {{"name", "probe"}, {"arguments", current_args}}}});

        bool duplicate = false;
        agent::AgentHooks hooks;
        hooks.on_tool_result = [&](const std::string&, const agent::ToolResult& r,
                                   const agent::json&) {
            duplicate = !r.ok && r.error.find("already ran") != std::string::npos;
        };
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx);
        return duplicate;
    };

    const agent::json repeat = {{"x", "1"}};
    ASSERT_TRUE(rejected(R"({"x":"1"})", "ok", true, repeat)); // wire-format string args
    ASSERT_TRUE(rejected(repeat, "ok", true, repeat));         // args already an object
    ASSERT_FALSE(rejected(repeat, "denied", true, repeat));    // denied: a retry is allowed
    ASSERT_FALSE(rejected({{"x", "2"}}, "ok", true, repeat));  // different arguments
    ASSERT_FALSE(rejected("not json", "ok", true, repeat));    // unparseable stored args
    ASSERT_FALSE(rejected(repeat, "ok", false, repeat));       // detection switched off
    ASSERT_TRUE(rejected(repeat, nullptr, true, repeat));      // no prior result is not a denial
}

TEST(dispatch_auto_approves_in_write_mode) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::ConversationLog log;
    reg.register_tool(agent::make_bash_tool());

    std::set<std::string> approved;

    int tool_results = 0;
    bool approval_called = false;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string& /*n*/, const agent::ToolResult& /*r*/,
                               const agent::json&) { ++tool_results; };
    hooks.on_approval = [&](const std::string& /*n*/, const agent::json&,
                            const std::string&) -> agent::Approval {
        approval_called = true;
        return agent::Approval::AllowSession;
    };

    agent::json calls = agent::json::array();
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "bash"},
                      {"arguments", {{"command", "rm -rf /tmp/amber_test_dispatch"}}}};
    calls.push_back(tc);

    agent::Context dctx;
    bool ok =
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx);
    ASSERT(ok);
    // Write mode consults the approval callback for gated tools
    ASSERT(approval_called);
    ASSERT(tool_results == 1);
    // AllowSession stores the grant
    ASSERT(approved.count("bash") == 1);
}

TEST(dispatch_missing_tool_reports_unknown) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::ConversationLog log;
    std::set<std::string> approved;

    int tool_results = 0;
    agent::ToolResult captured;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string&, const agent::ToolResult& r, const agent::json&) {
        ++tool_results;
        captured = r;
    };

    agent::json calls = agent::json::array();
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "nonexistent_tool"}, {"arguments", "{}"}};
    calls.push_back(tc);

    agent::Context dctx;
    bool ok =
        agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr, &dctx);
    ASSERT_FALSE(ok);
    ASSERT(tool_results == 1);
    ASSERT_FALSE(captured.ok);
    ASSERT(captured.error.find("unknown") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Context compression tests
// ---------------------------------------------------------------------------

static agent::Message msg(const std::string& role, const std::string& content,
                          const std::string& name = "") {
    return {role, content, "", "", name, json::object(), false, ""};
}

// An assistant message carrying one tool call, with the id a result must echo.
static agent::Message tool_call_msg(const std::string& id, const std::string& tool,
                                    const std::string& args = "{}") {
    agent::Message m = msg("assistant", "");
    m.tool_calls = json::array();
    m.tool_calls.push_back(
        {{"id", id}, {"type", "function"}, {"function", {{"name", tool}, {"arguments", args}}}});
    return m;
}

// The result of a tool call, carrying the id the model's call declared.
static agent::Message tool_result_msg(const std::string& id, const std::string& content) {
    agent::Message m = msg("tool", content, "read");
    m.tool_call_id = id;
    return m;
}

// Provider-valid pairing: a tool result must follow an assistant message that
// declares its id. The harness's own repair pass only checks the message kind,
// so this is the stricter check a real endpoint applies.
static std::size_t orphan_tool_results(const std::vector<agent::Message>& hist) {
    std::size_t orphans = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        if (hist[i].role != "tool")
            continue;
        bool matched = false;
        if (i > 0 && hist[i - 1].role == "assistant" && hist[i - 1].tool_calls.is_array())
            for (const auto& call : hist[i - 1].tool_calls)
                if (call.contains("id") && call["id"] == hist[i].tool_call_id)
                    matched = true;
        if (!matched)
            ++orphans;
    }
    return orphans;
}

// The other half of the pairing contract: every declared call has a result.
static std::size_t dangling_tool_calls(const std::vector<agent::Message>& hist) {
    std::size_t dangling = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        if (hist[i].role != "assistant" || !hist[i].tool_calls.is_array())
            continue;
        for (const auto& call : hist[i].tool_calls) {
            if (!call.contains("id"))
                continue;
            bool answered = false;
            for (std::size_t j = i + 1; j < hist.size() && hist[j].role == "tool"; ++j)
                if (hist[j].tool_call_id == call["id"])
                    answered = true;
            if (!answered)
                ++dangling;
        }
    }
    return dangling;
}

static bool has_real_user(const std::vector<agent::Message>& hist) {
    for (const auto& m : hist)
        if (m.role == "user" && !agent::is_harness_notice(m))
            return true;
    return false;
}

static agent::Message compressed_ctx_msg() {
    agent::Message ctx;
    ctx.role = "system";
    ctx.content = "Compressed conversation context:\n{\"archive\":[]}";
    return ctx;
}

// =========================================================================
// Scanner tests
// =========================================================================

TEST(collapse_loops_noop_on_short_history) {
    std::vector<agent::Message> hist = {
        msg("user", "hello"),
        msg("assistant", "hi"),
    };
    auto before = hist.size();
    agent::collapse_loops(hist);
    ASSERT(hist.size() == before);
}

TEST(collapse_loops_noop_on_no_loop) {
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),       msg("user", "do something"), msg("assistant", "ok"),
        msg("tool", "output", "read"), msg("assistant", "done"),    msg("user", "next"),
    };
    auto before = hist.size();
    agent::collapse_loops(hist);
    ASSERT(hist.size() == before);
}

TEST(collapse_loops_removes_tool_loop) {
    json tc = json::array();
    json fn;
    fn["function"] = {{"name", "read"}, {"arguments", "file.txt"}};
    tc.push_back(fn);
    agent::Message loop_msg;
    loop_msg.role = "assistant";
    loop_msg.tool_calls = tc;
    loop_msg.content = "";

    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "read file.txt"),
        loop_msg,
        msg("tool", "file content", "read"),
        loop_msg,
        msg("tool", "file content", "read"),
        loop_msg,
        msg("tool", "file content", "read"),
        msg("assistant", "done."),
    };
    auto before = hist.size();
    agent::collapse_loops(hist);
    // Should have removed the 3 loop_msg + 3 tool results, inserted 1 note
    ASSERT(hist.size() < before);
    bool has_note = false;
    for (const auto& m : hist)
        if (m.content.find("[loop collapsed]") != std::string::npos)
            has_note = true;
    ASSERT(has_note);
}

// =========================================================================
// Parser tests
// =========================================================================

TEST(parse_compression_response_empty) {
    auto cr = agent::parse_compression_response("");
    ASSERT(cr.segments.empty());
    ASSERT(cr.memory_ops.empty());
}

TEST(parse_compression_response_invalid_json) {
    auto cr = agent::parse_compression_response("not json");
    ASSERT(cr.segments.empty());
}

// extract_json_block recovers JSON from a chatty model response. It had no test at
// all until the patch-coverage gate flagged its branches during a refactor -- a
// function nobody exercised is a function nobody changed safely.
TEST(parse_compression_response_strips_code_fences) {
    auto cr = agent::parse_compression_response(
        "Here is the result:\n```json\n{\"classification\":[{\"turns\":\"0-0\","
        "\"tag\":\"core\",\"summary\":\"x\"}]}\n```\nHope that helps.");
    ASSERT(cr.segments.size() == 1);
    ASSERT(cr.segments[0].tag == agent::Classification::core);
}

// The stage that slices from the first '{' to the last '}' -- the one that copes
// with prose both before and after the JSON.
TEST(parse_compression_response_slices_trailing_prose) {
    auto cr = agent::parse_compression_response(
        "Sure! {\"classification\":[{\"turns\":\"0-0\",\"tag\":\"prune\","
        "\"summary\":\"y\"}]} -- let me know if you need more.");
    ASSERT(cr.segments.size() == 1);
    ASSERT(cr.segments[0].tag == agent::Classification::prune);
}

// A bare array is the classification-only shape.
TEST(parse_compression_response_bare_array) {
    auto cr = agent::parse_compression_response(
        "[{\"turns\":\"0-0\",\"tag\":\"core\",\"summary\":\"z\"}]");
    ASSERT(cr.segments.size() == 1);
}

// Nothing that looks like JSON at all: every stage declines.
TEST(parse_compression_response_no_json_present) {
    auto cr = agent::parse_compression_response("I cannot help with that request.");
    ASSERT(cr.segments.empty());
    ASSERT(cr.memory_ops.empty());
}

// Braces that open but never close: the slice stage finds no closing brace.
TEST(parse_compression_response_unterminated_object) {
    auto cr = agent::parse_compression_response("{\"classification\":[");
    ASSERT(cr.segments.empty());
}

TEST(parse_compression_response_valid) {
    std::string payload = R"({
        "classification": [
            {"turns": "0-0", "tag": "core", "summary": ""},
            {"turns": "1-3", "tag": "context", "summary": "explored layout"},
            {"turns": "4-5", "tag": "prune", "summary": ""}
        ],
        "memories": [
            {"content": "project uses make", "tags": ["build"], "action": "upsert"}
        ],
        "skills": [
            {"content": "run make test", "tags": ["test"], "trigger_phrase": "test", "action": "upsert"}
        ]
    })";
    auto cr = agent::parse_compression_response(payload);
    ASSERT(cr.segments.size() == 3u);
    ASSERT(cr.segments[0].tag == agent::Classification::core);
    ASSERT(cr.segments[1].tag == agent::Classification::context);
    ASSERT(cr.segments[1].summary == "explored layout");
    ASSERT(cr.segments[2].tag == agent::Classification::prune);
    ASSERT(cr.memory_ops.size() == 1u);
    ASSERT(cr.memory_ops[0].content == "project uses make");
    ASSERT(cr.memory_ops[0].action == "upsert");
    ASSERT(cr.skill_ops.size() == 1u);
    ASSERT(cr.skill_ops[0].content == "run make test");
}

// A model emitting null where a string belongs used to cost the whole
// classification: json::value() throws type_error.302 on a present-but-null
// key. One malformed field must cost only that field.
TEST(parse_compression_response_null_fields_are_survivable) {
    std::string payload = R"({
        "summary": null,
        "classification": [
            {"turns": "0-5", "tag": null, "summary": null}
        ],
        "memories": [
            {"name": null, "content": "kept", "tags": [null, 7, "area"], "action": null}
        ],
        "brief": {"intent": null, "next": "ship it", "done": [null, "one"]}
    })";
    auto cr = agent::parse_compression_response(payload);
    ASSERT(cr.segments.size() == 1u);
    ASSERT(cr.segments[0].tag == agent::Classification::context);
    ASSERT(cr.segments[0].summary.empty());
    ASSERT(cr.summary.empty());
    ASSERT(cr.memory_ops.size() == 1u);
    ASSERT(cr.memory_ops[0].name.empty());
    ASSERT(cr.memory_ops[0].content == "kept");
    ASSERT(cr.memory_ops[0].action == "upsert");
    ASSERT(cr.memory_ops[0].tags.size() == 1u);
    ASSERT(cr.memory_ops[0].tags[0] == "area");
    ASSERT(cr.brief.has_value());
    ASSERT(cr.brief->next == "ship it");
    ASSERT(cr.brief->done.size() == 1u);
}

// A fenced reply is the case the fence stripper exists for. It used to delete
// the two characters `\n` anywhere in the text, so an escaped newline inside a
// valid JSON string was silently rewritten ("line oneline two") and an escaped
// backslash before an `n` could become an invalid escape that cost the whole
// classification.
TEST(parser_survives_escaped_newlines_in_a_fenced_reply) {
    const std::string payload =
        "```json\n"
        R"({"summary":"line one\nline two","classification":[{"turns":"1-2","tag":"prune"}]})"
        "\n```";
    auto cr = agent::parse_compression_response(payload);
    ASSERT_EQ(cr.segments.size(), 1u);
    ASSERT_EQ(cr.summary, std::string("line one\nline two"));
}

TEST(parser_preserves_escaped_backslashes_in_a_fenced_reply) {
    const std::string payload =
        "```json\n"
        R"({"summary":"see C:\\new\\file.txt","classification":[{"turns":"1-2","tag":"core"}]})"
        "\n```";
    auto cr = agent::parse_compression_response(payload);
    ASSERT_EQ(cr.segments.size(), 1u);
    ASSERT(cr.summary.find("C:\\new\\file.txt") != std::string::npos);
}

TEST(parser_keeps_fence_tokens_inside_a_string) {
    // A summary may quote the very syntax the stripper looks for. Only a fence
    // outside the JSON value is a fence.
    const std::string payload =
        "```json\n"
        R"({"summary":"wrap code in ``` fences","classification":[{"turns":"1-2","tag":"core"}]})"
        "\n```";
    auto cr = agent::parse_compression_response(payload);
    ASSERT_EQ(cr.segments.size(), 1u);
    ASSERT_EQ(cr.summary, std::string("wrap code in ``` fences"));
}

// The classifier cannot count messages it only sees as text. The instruction
// carries an index legend whose numbers match the harness's message indices —
// the same ones apply_classification uses.
TEST(classifier_manifest_indices_match_the_harness_message_indices) {
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "first task"),
        msg("assistant", "working"),
        msg("user", "current task"),
    };
    const auto req = agent::build_classify_request(false, hist);
    ASSERT_EQ(req.role, std::string("user"));
    ASSERT(req.content.find("Index 0 is the system prompt") != std::string::npos);
    ASSERT(req.content.find("1 user first task") != std::string::npos);
    ASSERT(req.content.find("2 assistant working") != std::string::npos);
    ASSERT(req.content.find("3 user current task") != std::string::npos);
}

TEST(classifier_manifest_is_bounded_and_groups_the_oldest) {
    // 400 messages must not turn the instruction into a second conversation:
    // the oldest are grouped, the newest stay individually addressable.
    std::vector<agent::Message> hist = {msg("system", "Amber")};
    for (int i = 0; i < 400; ++i)
        hist.push_back(msg("tool", std::string(120, 'x'), "read"));
    const auto req = agent::build_classify_request(false, hist);
    ASSERT(req.content.find("(grouped") != std::string::npos);
    ASSERT(agent::message_tokens(req) < 2600u);
}

TEST(update_instruction_names_the_archive_wire_position) {
    // The archive is merged into the leading system prompt by every dialect, so
    // "the message above" was never where the model saw it.
    std::vector<agent::Message> hist = {msg("system", "Amber"), msg("user", "task")};
    const auto req = agent::build_classify_request(true, hist);
    ASSERT(req.content.find("merged into") != std::string::npos);
    ASSERT(req.content.find("numbered transcript") != std::string::npos);
}

// =========================================================================
// Applier tests
// =========================================================================

TEST(apply_classification_empty) {
    auto result = agent::apply_classification({}, agent::CompressionResponse{});
    ASSERT(result.empty());
}

TEST(apply_classification_all_core) {
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "hello"),
        msg("assistant", "hi"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({0, 0, agent::Classification::core, ""});
    cr.segments.push_back({1, 1, agent::Classification::core, ""});
    cr.segments.push_back({2, 2, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    // All core + archive system msg appended
    ASSERT(result.size() >= hist.size());
}

TEST(apply_classification_prunes_and_archives) {
    // Four user exchanges; the recent-turn safety net protects the last two,
    // so an early prunable exchange is still removed.
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),     msg("user", "explore file a"),
        msg("assistant", ""),        msg("tool", std::string(200, 'x'), "read"),
        msg("assistant", "done a"),  msg("user", "explore file b"),
        msg("assistant", "done b"),  msg("user", "explore file c"),
        msg("assistant", "done c"),  msg("user", "active task"),
        msg("assistant", "working"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({1, 1, agent::Classification::core, ""});
    cr.segments.push_back({2, 2, agent::Classification::prune, ""});
    cr.segments.push_back({3, 3, agent::Classification::prune, ""});
    cr.segments.push_back({4, 10, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    // 2 pruned → result should be smaller than original
    ASSERT(result.size() < hist.size());
}

TEST(apply_classification_context_creates_archive_entry) {
    // The archived exchange is the oldest one — outside the recent-turn
    // safety net, so it is replaced by an archive entry as classified.
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),  msg("user", "do something"), msg("assistant", "working"),
        msg("user", "continue"),  msg("assistant", "ok"),      msg("user", "next"),
        msg("assistant", "sure"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({0, 0, agent::Classification::core, ""});
    cr.segments.push_back(
        {1, 2, agent::Classification::context, "user asked and assistant worked"});
    cr.segments.push_back({3, 6, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    // Should contain system msg + core turns + archive system msg
    ASSERT(result.size() >= 2u);
    bool has_archive = false;
    for (const auto& m : result)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            has_archive = true;
    ASSERT(has_archive);
}

TEST(archive_entries_are_capped_and_summarised) {
    // The archive is bookkeeping, not content: 40 carried entries must not ride
    // into every later request forever. The oldest merge into one line.
    agent::json entries = agent::json::array();
    for (int i = 0; i < 40; ++i)
        entries.push_back({{"turns", std::to_string(i)}, {"summary", "archived work"}});
    agent::json old_ctx = {{"archive", entries}};
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("system", std::string(agent::kCompressedContextPrefix) + "\n" + old_ctx.dump()),
        msg("user", "old task"),
        msg("assistant", "old work"),
        msg("user", "current task"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({2, 3, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    agent::json body;
    for (const auto& m : result)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            body = agent::json::parse(m.content.substr(sizeof(agent::kCompressedContextPrefix) - 1),
                                      nullptr, false);
    ASSERT(body.is_object());
    ASSERT(body["archive"].size() <= 32u);
    ASSERT(body["archive"][0]["summary"].get<std::string>().find("earlier") != std::string::npos);
}

TEST(archive_fold_candidates_are_the_oldest_entries_over_the_cap) {
    // Folding is a summary, not a delete: the newest entries stay verbatim, and
    // an archive inside its cap has nothing worth a request.
    agent::json archive = agent::json::array();
    for (int i = 0; i < 40; ++i)
        archive.push_back({{"turns", std::to_string(i)}, {"summary", "work " + std::to_string(i)}});

    const agent::json candidates = agent::archive_fold_candidates(archive);
    ASSERT_EQ(candidates.size(), 40u - agent::kArchiveKeepVerbatim);
    ASSERT_EQ(candidates.front()["summary"], std::string("work 0"));
    ASSERT_EQ(candidates.back()["summary"], std::string("work 31"));

    agent::json small = agent::json::array();
    for (int i = 0; i < 6; ++i)
        small.push_back({{"turns", std::to_string(i)}, {"summary", "work"}});
    ASSERT(agent::archive_fold_candidates(small).empty());

    // The token budget is the second trigger: an entry count the cap is happy
    // with, and summaries fat enough to break the token budget on their own.
    agent::json fat = agent::json::array();
    for (int i = 0; i < 12; ++i)
        fat.push_back({{"turns", std::to_string(i)}, {"summary", std::string(2000, 'f')}});
    ASSERT(fat.size() < agent::kMaxArchiveEntries);
    ASSERT(!agent::archive_fold_candidates(fat).empty());

    // An archive whose entries all fit the verbatim window has nothing older to
    // fold -- the merge cap is the fallback there, not a summary.
    agent::json few = agent::json::array();
    for (int i = 0; i < 5; ++i)
        few.push_back({{"turns", std::to_string(i)}, {"summary", std::string(2000, 'f')}});
    ASSERT(agent::archive_fold_candidates(few).empty());
}

TEST(archive_fold_is_one_entry_plus_the_newest_verbatim) {
    agent::json archive = agent::json::array();
    for (int i = 0; i < 20; ++i)
        archive.push_back({{"turns", std::to_string(i)}, {"summary", "work " + std::to_string(i)}});
    const agent::json folded =
        agent::apply_archive_fold(archive, "folded: early rounds", agent::kArchiveKeepVerbatim);
    ASSERT_EQ(folded.size(), agent::kArchiveKeepVerbatim + 1);
    ASSERT_EQ(folded.front()["summary"], std::string("folded: early rounds"));
    ASSERT_EQ(folded[1]["summary"], std::string("work 12"));
    ASSERT_EQ(folded.back()["summary"], std::string("work 19"));
    ASSERT(folded.dump().size() < archive.dump().size());
}

TEST(archive_fold_is_discarded_when_it_does_not_shrink) {
    // The monotonic guard: a "summary" that costs as much as the entries it
    // replaced is not a summary, and replacing them loses the detail for free.
    agent::json archive = agent::json::array();
    for (int i = 0; i < 20; ++i)
        archive.push_back({{"turns", std::to_string(i)}, {"summary", "w" + std::to_string(i)}});
    const std::string before = archive.dump();
    ASSERT_EQ(
        agent::apply_archive_fold(archive, std::string(4000, 'x'), agent::kArchiveKeepVerbatim)
            .dump(),
        before);
    ASSERT_EQ(agent::apply_archive_fold(archive, "", agent::kArchiveKeepVerbatim).dump(), before);
}

TEST(archive_fold_request_carries_the_entries_and_the_budget) {
    agent::json entries = agent::json::array();
    entries.push_back({{"turns", "1-4"}, {"summary", "the auth token lives in config.cpp"}});
    entries.push_back({{"turns", "5-9"}, {"summary", "the retry budget lives in policy.cpp"}});
    const agent::Message req = agent::build_archive_fold_request(entries);
    ASSERT(!req.content.empty());
    ASSERT(req.content.find("the auth token lives in config.cpp") != std::string::npos);
    ASSERT(req.content.find("the retry budget lives in policy.cpp") != std::string::npos);
    // One entry, and it has to be small: the point is to spend fewer tokens.
    ASSERT(req.content.find("one") != std::string::npos);
    ASSERT(req.content.find("shorter") != std::string::npos);
}

TEST(archive_fold_response_is_plain_text) {
    ASSERT_EQ(agent::parse_archive_fold_response("  folded work  \n"), std::string("folded work"));
    ASSERT_EQ(agent::parse_archive_fold_response("```\nfolded work\n```"),
              std::string("folded work"));
    ASSERT_EQ(agent::parse_archive_fold_response("   \n"), std::string());
}

TEST(compressed_context_message_has_no_write_only_fields) {
    // `type`, `version` and `facts` had no reader in lib/, tui/, src/ or bench/.
    // `facts.last_goal` was also the *first* short user message, not the last.
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "old task"),
        msg("assistant", "old work"),
        msg("user", "current task"),
    };
    agent::CompressionResponse cr;
    cr.summary = "the work state";
    cr.segments.push_back({1, 2, agent::Classification::context, "old work"});
    agent::CompressionConfig cfg;
    cfg.keep_last_prompts = 1;
    auto out = agent::apply_classification(hist, cr, &cfg);
    agent::json body;
    for (const auto& m : out)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            body = agent::json::parse(m.content.substr(sizeof(agent::kCompressedContextPrefix) - 1),
                                      nullptr, false);
    ASSERT(body.is_object());
    ASSERT_FALSE(body.contains("type"));
    ASSERT_FALSE(body.contains("version"));
    ASSERT_FALSE(body.contains("facts"));
    ASSERT_EQ(body["summary"], std::string("the work state"));
    ASSERT(body.contains("archive"));
}

TEST(apply_classification_preserves_system_when_classifier_tags_as_prune) {
    std::vector<agent::Message> hist = {
        msg("system", "Your name is Amber."),
        msg("user", "hello"),
        msg("assistant", "hi"),
    };
    // The LLM classifies turn 0 (index 0, the system prompt) as "prune" —
    // the old code would remove it outright.
    agent::CompressionResponse cr;
    cr.segments.push_back({0, 0, agent::Classification::prune, ""});
    cr.segments.push_back({1, 1, agent::Classification::core, ""});
    cr.segments.push_back({2, 2, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    // System prompt must survive at index 0
    ASSERT(!result.empty());
    ASSERT(result[0].role == "system");
    ASSERT(result[0].content.find("Amber") != std::string::npos);
}

TEST(apply_classification_preserves_system_when_classifier_tags_as_context) {
    std::vector<agent::Message> hist = {
        msg("system", "Your name is Amber."),
        msg("user", "hello"),
        msg("assistant", "hi"),
    };
    // The LLM classifies turn 0 (index 0, the system prompt) as "context" —
    // the old code would archive it, losing the identity.
    agent::CompressionResponse cr;
    cr.segments.push_back({0, 0, agent::Classification::context, "first turn"});
    cr.segments.push_back({1, 2, agent::Classification::core, ""});
    auto result = agent::apply_classification(hist, cr);
    // System prompt must survive at index 0
    ASSERT(!result.empty());
    ASSERT(result[0].role == "system");
    ASSERT(result[0].content.find("Amber") != std::string::npos);
}

// =========================================================================
// Compression gate tests (unchanged)
// =========================================================================

TEST(apply_classification_keeps_recent_turns_verbatim) {
    // Safety net: even when the classifier tags the recent turns "prune",
    // the last two user turns and everything after them must survive —
    // a misclassification must never drop the active task.
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),      msg("user", "task one"),       msg("assistant", "done one"),
        msg("user", "task two"),     msg("assistant", "doing two"), msg("user", "current task"),
        msg("assistant", "working"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({1, 6, agent::Classification::prune, ""});
    auto result = agent::apply_classification(hist, cr);
    bool kept_current = false;
    for (const auto& m : result)
        if (m.content.find("current task") != std::string::npos)
            kept_current = true;
    ASSERT(kept_current);
    bool kept_previous = false;
    for (const auto& m : result)
        if (m.content.find("task two") != std::string::npos)
            kept_previous = true;
    ASSERT(kept_previous);
}

TEST(apply_classification_carries_previous_archive) {
    // Re-compression updates the archive: entries from the previous
    // compressed-context message survive beside the new ones, and the old
    // compressed message is consumed (never duplicated in the output).
    agent::json old_archive = agent::json::array();
    old_archive.push_back({{"turns", "0-2"}, {"summary", "old work"}});
    agent::json old_ctx;
    old_ctx["type"] = "compressed_context";
    old_ctx["version"] = 1;
    old_ctx["archive"] = old_archive;
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("system", std::string(agent::kCompressedContextPrefix) + "\n" + old_ctx.dump()),
        msg("user", "middle task"),
        msg("assistant", "mid work"),
        msg("user", "later task"),
        msg("assistant", "later work"),
        msg("user", "current task"),
        msg("assistant", "working"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({2, 3, agent::Classification::context, "new investigation"});
    auto result = agent::apply_classification(hist, cr);
    std::string body;
    for (const auto& m : result)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            body = m.content;
    ASSERT_FALSE(body.empty());
    auto j = agent::json::parse(body.substr(sizeof(agent::kCompressedContextPrefix) - 1), nullptr,
                                false);
    ASSERT(j.is_object());
    ASSERT_EQ(j["archive"].size(), 2u); // old entry + new entry
    bool old_found = false, new_found = false;
    for (const auto& e : j["archive"]) {
        if (e["summary"] == "old work")
            old_found = true;
        if (e["summary"] == "new investigation")
            new_found = true;
    }
    ASSERT(old_found);
    ASSERT(new_found);
    // The old compressed-context message must not survive separately.
    int prefix_count = 0;
    for (const auto& m : result)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            ++prefix_count;
    ASSERT_EQ(prefix_count, 1);
}

TEST(apply_classification_truncates_long_summaries) {
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),      msg("user", "middle task"),     msg("assistant", "mid work"),
        msg("user", "later task"),   msg("assistant", "later work"), msg("user", "current task"),
        msg("assistant", "working"),
    };
    agent::CompressionResponse cr;
    cr.segments.push_back({1, 2, agent::Classification::context, std::string(500, 'x')});
    auto result = agent::apply_classification(hist, cr);
    std::string body;
    for (const auto& m : result)
        if (m.content.compare(0, sizeof(agent::kCompressedContextPrefix) - 1,
                              agent::kCompressedContextPrefix) == 0)
            body = m.content;
    ASSERT_FALSE(body.empty());
    auto j = agent::json::parse(body.substr(sizeof(agent::kCompressedContextPrefix) - 1), nullptr,
                                false);
    ASSERT(j.is_object());
    ASSERT_EQ(j["archive"].size(), 1u);
    std::string summary = j["archive"][0]["summary"].get<std::string>();
    ASSERT(summary.size() <= agent::kMaxSummaryChars);
}

TEST(sanitize_tool_pairs_drops_orphan_tool_message) {
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "task"),
        msg("tool", "orphan result"), // no preceding assistant tool_calls
        msg("user", "next"),
    };
    agent::sanitize_tool_pairs(hist);
    ASSERT_EQ(hist.size(), 3u);
    ASSERT_EQ(hist[1].role, "user");
    ASSERT_EQ(hist[2].role, "user");
}

TEST(sanitize_tool_pairs_stubs_missing_result) {
    agent::Message call;
    call.role = "assistant";
    call.tool_calls = json::array();
    call.tool_calls.push_back({{"id", "call_1"},
                               {"type", "function"},
                               {"function", {{"name", "bash"}, {"arguments", "{}"}}}});
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "task"),
        call, // tool_calls whose result was pruned
        msg("user", "next"),
    };
    agent::sanitize_tool_pairs(hist);
    ASSERT_EQ(hist.size(), 5u);
    ASSERT_EQ(hist[2].role, "assistant"); // the tool_calls message
    ASSERT_EQ(hist[3].role, "tool");      // stub injected before the user turn
    ASSERT_EQ(hist[4].role, "user");
}

TEST(subagent_tail_is_bounded) {
    // A tool-level live tail: newest lines survive, and a chatty sub-agent
    // cannot grow its viewer without limit.
    std::string tail;
    for (int i = 0; i < 500; ++i)
        agent::append_task_tail(tail, "line " + std::to_string(i));
    std::size_t lines = 0;
    for (char c : tail)
        if (c == '\n')
            ++lines;
    ASSERT(lines <= agent::kTaskTailMaxLines);
    ASSERT(tail.find("line 499") != std::string::npos);
    ASSERT(tail.find("line 0\n") == std::string::npos);

    std::string wide;
    for (int i = 0; i < 50; ++i)
        agent::append_task_tail(wide, std::string(500, 'x'));
    ASSERT(wide.size() <= agent::kTaskTailMaxBytes);
}

TEST(shape_plan_digests_a_live_result_and_keeps_it_recallable) {
    // The model names evidence; the harness replaces it with a digest that still
    // carries the handle, so the payload is a read away, not gone.
    XdgGuard xdg("shape_drop");
    agent::ToolResult r;
    r.ok = true;
    r.output = std::string(4000, 's');
    const std::string plain = agent::format_tool_envelope("read", json::object(), r, 32 * 1024);
    const std::string id = agent::store_digested_artifact(r.output);
    ASSERT_EQ(id.size(), 16u);
    r.meta = {{"result_id", id}};
    const std::string env = agent::format_tool_envelope("read", json::object(), r, 32 * 1024);
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),     msg("user", "go"),        tool_call_msg("c1", "read"),
        tool_result_msg("c1", env), msg("assistant", "done"),
    };
    agent::ShapePlan plan;
    plan.op = agent::ShapeOp::Drop;
    plan.result_ids = {id};
    const agent::ShapeOutcome out = agent::apply_shape_plan(hist, plan);
    ASSERT_EQ(out.applied, 1u);
    ASSERT(out.freed_bytes > 0u);
    ASSERT(hist[3].content.rfind("[tool output omitted", 0) == 0);
    ASSERT(hist[3].content.find("result_id=" + id) != std::string::npos);
    std::string error;
    const auto original = agent::read_tool_artifact(id, 0, 8192, "", error);
    ASSERT(original.has_value());
    ASSERT_EQ(*original, r.output); // the paged payload, byte for byte
}

TEST(shape_plan_rejects_an_id_that_is_not_live) {
    // An id the context no longer carries is reported, not guessed at.
    XdgGuard xdg("shape_reject");
    std::vector<agent::Message> hist = {msg("system", "Amber"), msg("user", "go")};
    agent::ShapePlan plan;
    plan.result_ids = {"00000000000000ff"};
    const agent::ShapeOutcome out = agent::apply_shape_plan(hist, plan);
    ASSERT_EQ(out.applied, 0u);
    ASSERT_EQ(out.rejected.size(), 1u);
    ASSERT_EQ(out.rejected.front(), std::string("00000000000000ff"));
}

TEST(shape_plan_superseded_drops_only_the_earlier_read) {
    // Read the same file twice: the first read is stale, the second is the one
    // the task stands on.
    XdgGuard xdg("shape_superseded");
    const auto read_env = [](const std::string& body) {
        agent::ToolResult r;
        r.ok = true;
        r.output = body;
        return agent::format_tool_envelope("read", {{"path", "Makefile"}}, r, 32 * 1024);
    };
    const std::string first = read_env(std::string(4000, 'a'));
    const std::string second = read_env(std::string(4000, 'b'));
    const std::string id1 = agent::store_digested_artifact(first);
    const std::string id2 = agent::store_digested_artifact(second);
    ASSERT_EQ(id1.size(), 16u);
    ASSERT_EQ(id2.size(), 16u);
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),       msg("user", "go"),           tool_call_msg("c1", "read"),
        tool_result_msg("c1", first), tool_call_msg("c2", "read"), tool_result_msg("c2", second),
    };
    agent::ShapePlan plan;
    plan.op = agent::ShapeOp::Superseded;
    const agent::ShapeOutcome out = agent::apply_shape_plan(hist, plan);
    ASSERT_EQ(out.applied, 1u);
    ASSERT(hist[3].content.rfind("[tool output omitted", 0) == 0);
    ASSERT_EQ(hist[5].content, second);
}

TEST(shape_queue_drains_once) {
    agent::ShapeQueue queue;
    ASSERT(queue.empty());
    agent::ShapePlan first;
    first.result_ids = {"aa"};
    agent::ShapePlan second;
    second.op = agent::ShapeOp::Superseded;
    queue.push(first);
    queue.push(second);
    ASSERT(!queue.empty());
    const std::vector<agent::ShapePlan> drained = queue.drain();
    ASSERT_EQ(drained.size(), 2u);
    ASSERT(queue.empty());
    ASSERT(queue.drain().empty());
}

TEST(context_tool_is_read_only) {
    // The READ-mode gate denies every non-read-only tool. Shaping changes no
    // workspace file and every replacement is recallable, so denying it there
    // only takes the model's trim away in the mode most sessions start in --
    // measured live, where the model asked for superseded reads and got
    // "denied".
    agent::ShapeQueue queue;
    const auto tool = agent::make_context_tool(queue);
    ASSERT(tool->is_read_only());
    ASSERT(!tool->requires_approval(agent::json::object()));
}

TEST(context_tool_queues_a_drop_plan) {
    // The tool's whole job is to put a plan where the agent will find it. It
    // never touches the context: the payload's fate is decided at the boundary,
    // against the history that is there then.
    agent::ShapeQueue queue;
    const auto tool = agent::make_context_tool(queue);
    ASSERT_EQ(tool->name(), std::string("context"));
    agent::json args = {
        {"op", "drop"}, {"result_ids", {"0123456789abcdef"}}, {"reason", "read twice"}};
    const agent::ToolResult r = tool->execute(args);
    ASSERT(r.ok);
    ASSERT(r.output.find("queued") != std::string::npos);
    ASSERT(!queue.empty());
    const std::vector<agent::ShapePlan> plans = queue.drain();
    ASSERT_EQ(plans.size(), 1u);
    ASSERT(plans.front().op == agent::ShapeOp::Drop);
    ASSERT_EQ(plans.front().result_ids.size(), 1u);
    ASSERT_EQ(plans.front().result_ids.front(), std::string("0123456789abcdef"));
    ASSERT_EQ(plans.front().reason, std::string("read twice"));
}

TEST(context_tool_superseded_needs_no_ids) {
    // The agent can ask for the repeated-read set without naming it: the rule
    // lives in the harness, the trigger lives here.
    agent::ShapeQueue queue;
    const auto tool = agent::make_context_tool(queue);
    const agent::ToolResult r = tool->execute(agent::json{{"op", "superseded"}});
    ASSERT(r.ok);
    const std::vector<agent::ShapePlan> plans = queue.drain();
    ASSERT_EQ(plans.size(), 1u);
    ASSERT(plans.front().op == agent::ShapeOp::Superseded);
    ASSERT(plans.front().result_ids.empty());
}

TEST(context_tool_refuses_a_drop_without_ids) {
    agent::ShapeQueue queue;
    const auto tool = agent::make_context_tool(queue);
    const agent::ToolResult r = tool->execute(agent::json{{"op", "drop"}});
    ASSERT(!r.ok);
    ASSERT(r.error.find("result_ids") != std::string::npos);
    ASSERT(queue.empty());
}

TEST(context_tool_refuses_an_unknown_op) {
    agent::ShapeQueue queue;
    const auto tool = agent::make_context_tool(queue);
    const agent::ToolResult r = tool->execute(agent::json{{"op", "edit"}});
    ASSERT(!r.ok);
    ASSERT(r.error.find("op") != std::string::npos);
    ASSERT(queue.empty());
}

TEST(prune_tool_io_removes_bulky_output_outside_tail) {
    XdgGuard xdg("prune_outside_tail");
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "task a"),
        msg("assistant", "a done"),
        msg("tool", std::string(500, 'a'), "bash"), // bulky, old
        msg("user", "task b"),
        msg("assistant", "b done"),
        msg("tool", "short", "bash"), // short, old
        msg("user", "current task"),
        msg("assistant", "working"),
        msg("tool", std::string(500, 'b'), "bash"), // bulky, in tail
    };
    agent::prune_tool_io(hist);
    size_t omitted = 0;
    for (const auto& m : hist) {
        if (m.content.find("[tool output omitted") != std::string::npos)
            ++omitted;
    }
    ASSERT_EQ(omitted, 1u);
    ASSERT(hist[3].content.size() < 300u);  // pruned to a digest, not the payload
    ASSERT_EQ(hist[6].content, "short");    // kept
    ASSERT(hist[9].content.size() == 500u); // tail kept
}

TEST(prune_tool_io_keeps_the_last_tool_result_of_the_active_turn) {
    XdgGuard xdg("prune_active_turn");
    // The pipeline runs the prune at the top of every request, i.e. right after
    // tool results were pushed. The current turn's evidence must survive; only
    // output the agent has moved past may be replaced by a digest.
    agent::CompressionConfig cc;
    cc.keep_last_prompts = 1;
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "old task"),
        tool_call_msg("c1", "read"),
        tool_result_msg("c1", std::string(600, 'o')), // old: prunable
        msg("user", "current task"),
        tool_call_msg("c2", "read"),
        tool_result_msg("c2", std::string(600, 'n')), // active turn: must survive
        tool_call_msg("c3", "bash"),
        tool_result_msg("c3", std::string(600, 'm')), // multi-call tail: must survive
    };
    agent::prune_tool_io(hist, &cc);
    ASSERT(hist[3].content.size() < 300u);
    ASSERT_EQ(hist[6].content, std::string(600, 'n'));
    ASSERT_EQ(hist[8].content, std::string(600, 'm'));
}

TEST(prune_tool_io_keeps_the_newest_result_inside_the_active_turn) {
    // The archive pass cannot touch the active turn, but the turn itself can
    // exceed the window. The newest evidence survives; older results of the same
    // turn become digests once the tail budget is spent.
    XdgGuard xdg("prune_in_turn_budget");
    agent::CompressionConfig cc;
    cc.context_size = 32768;
    std::vector<agent::Message> hist = {msg("system", "Amber"), msg("user", "one long task")};
    for (int r = 0; r < 20; ++r) {
        hist.push_back(tool_call_msg("c" + std::to_string(r), "read"));
        hist.push_back(tool_result_msg("c" + std::to_string(r),
                                       "r" + std::to_string(r) + std::string(8000, 'x')));
    }
    const std::size_t replaced = agent::prune_tool_io(hist, &cc);
    ASSERT(replaced >= 15u);
    ASSERT_EQ(hist[1].content, std::string("one long task"));
    ASSERT_EQ(hist.back().content, "r19" + std::string(8000, 'x'));
    ASSERT(hist.back().content.find("[tool output omitted") == std::string::npos);
}

TEST(prune_in_turn_results_needs_a_known_window) {
    // The gate never guesses a window; neither does in-turn pruning. With no
    // window there is no budget, so nothing is replaced.
    XdgGuard xdg("prune_in_turn_nowindow");
    agent::CompressionConfig cc; // context_size == 0
    std::vector<agent::Message> hist = {msg("system", "Amber"), msg("user", "task")};
    for (int r = 0; r < 3; ++r) {
        hist.push_back(tool_call_msg("c" + std::to_string(r), "bash"));
        hist.push_back(tool_result_msg("c" + std::to_string(r), std::string(4000, 'x')));
    }
    ASSERT_EQ(agent::prune_in_turn_results(hist, &cc), 0u);
    ASSERT_EQ(hist.back().content, std::string(4000, 'x'));
}

TEST(prune_tool_io_legacy_path_keeps_its_two_prompt_guard) {
    XdgGuard xdg("prune_legacy");
    // cfg == nullptr is the legacy direct call: it prunes only output before the
    // second-to-last user turn. That boundary is unchanged.
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", "task a"),
        msg("tool", std::string(600, 'a'), "bash"),
        msg("user", "task b"),
        msg("tool", std::string(600, 'b'), "bash"),
        msg("user", "task c"),
        msg("tool", std::string(600, 'c'), "bash"),
    };
    agent::prune_tool_io(hist);
    ASSERT(hist[2].content.size() < 300u); // before the second-to-last prompt
    ASSERT_EQ(hist[4].content, std::string(600, 'b'));
    ASSERT_EQ(hist[6].content, std::string(600, 'c'));
}

TEST(prune_tool_io_keeps_a_readable_digest) {
    // The generic placeholder lost the tool, status and size; the digest keeps
    // an index the model can act on without re-running the call.
    agent::ToolResult big;
    big.output = std::string(5000, 'a');
    big.meta = {{"result_id", "abc123"}};
    const std::string env = agent::format_tool_envelope("read", json::object(), big, 4096);
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),      msg("user", "task a"), msg("assistant", "a done"),
        msg("tool", env, "read"),    msg("user", "task b"), msg("assistant", "b done"),
        msg("user", "current task"),
    };
    std::vector<agent::Message> again = hist;
    const std::size_t replaced = agent::prune_tool_io(hist);
    ASSERT_EQ(replaced, 1u);
    const std::string& digest = hist[3].content;
    ASSERT(digest.find("[tool output omitted") == 0);
    ASSERT(digest.find("tool=read") != std::string::npos);
    ASSERT(digest.find("status=ok") != std::string::npos);
    ASSERT(digest.find("bytes=5000") != std::string::npos);
    ASSERT(digest.find("result_id=abc123") != std::string::npos);
    ASSERT(digest.size() < 400u);
    agent::prune_tool_io(again);
    ASSERT_EQ(again[3].content, digest); // deterministic
}

TEST(digested_result_is_recallable_by_id) {
    XdgGuard xdg("digest_recall_medium");
    agent::CompressionConfig cc;
    cc.keep_last_prompts = 1;
    std::string body = "[tool=read status=ok meta={}]\n";
    for (int i = 0; i < 400; ++i)
        body += "line " + std::to_string(i) + " of a file the agent already read\n";
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),      msg("user", "old task"),     tool_call_msg("c1", "read"),
        tool_result_msg("c1", body), msg("user", "current task"),
    };
    ASSERT_EQ(agent::prune_tool_io(hist, &cc), 1u);
    const std::string id = result_id_from_text(hist[3].content);
    ASSERT_EQ(id.size(), 16u); // today the digest carries no id at all
    ASSERT(agent::valid_artifact_id(id));
    std::string error;
    const auto stored = agent::read_tool_artifact(id, 0, 65536, "", error);
    ASSERT(stored.has_value());
    ASSERT_EQ(*stored, body);
}

TEST(read_result_serves_a_small_result_after_compaction) {
    XdgGuard xdg("digest_recall_small");
    agent::CompressionConfig cc;
    cc.keep_last_prompts = 1;
    const std::string body = std::string(250, 's'); // under the 4 KB store floor
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),      msg("user", "old task"),     tool_call_msg("c1", "bash"),
        tool_result_msg("c1", body), msg("user", "current task"),
    };
    ASSERT_EQ(agent::prune_tool_io(hist, &cc), 1u);
    const std::string id = result_id_from_text(hist[3].content);
    ASSERT_EQ(id.size(), 16u);
    const auto tool = agent::make_read_result_tool();
    const agent::ToolResult page =
        tool->execute({{"result_id", id}, {"offset", 0}, {"limit", 4096}});
    ASSERT(page.ok);
    ASSERT_EQ(page.output, body);
}

TEST(tool_result_digest_falls_back_for_a_plain_message) {
    // Messages recorded before the envelope existed (or by a tool that returns
    // raw text) still digest to something actionable, never to an empty line.
    const agent::Message plain = msg("tool", std::string(300, 'q'), "bash");
    const std::string digest = agent::tool_result_digest(plain);
    ASSERT(digest.find("tool=bash") != std::string::npos);
    ASSERT(digest.find("status=unknown") != std::string::npos);
    ASSERT(digest.find("bytes=300") != std::string::npos);
    ASSERT(digest.size() < 120u);
}

TEST(compression_gate_below_threshold) {
    agent::CompressionConfig cc;
    cc.threshold = 0.75;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 100000;
    agent::Context ctx;
    ctx.push(msg("system", "short"));
    ctx.push(msg("user", "hi"));
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_above_threshold) {
    agent::CompressionConfig cc;
    cc.threshold = 0.10;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 1000;
    agent::Context ctx;
    for (int i = 0; i < 10; ++i)
        ctx.push(msg("user", std::string(100, 'a')));
    cfg.turn_counter = 25;
    cfg.user_turn_counter = cfg.turn_counter; // past the 20-turn cooldown
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_min_turns) {
    agent::CompressionConfig cc;
    cc.threshold = 0.01;
    cc.min_turns = 100;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 100000;
    agent::Context ctx;
    ctx.push(msg("user", "hello"));
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_default_threshold_is_70_percent) {
    // Default: on by default, firing at 70% of the window.
    agent::CompressionConfig cc;
    cc.min_turns = 1;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 262144;
    cfg.turn_counter = 25;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 183000; // ~69.8%
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 184000; // ~70.2%
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_does_not_cap_large_window) {
    // Regression: the old unconditional 32k budget cap fired at ~16k tokens
    // on a 262k window ("compression at 8% of context"). The honest gate
    // must not fire there.
    agent::CompressionConfig cc;
    cc.min_turns = 1;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 262144;
    cfg.prompt_tokens_used = 30000;
    cfg.turn_counter = 25;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_unknown_window_never_fires) {
    // No window, no guessing: compression only runs on /compress or after
    // the server teaches the real limit via an overflow rejection.
    agent::CompressionConfig cc;
    cc.min_turns = 1;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg; // context_size == 0
    cfg.prompt_tokens_used = 100000;
    cfg.turn_counter = 25;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
}

// ---------------------------------------------------------------------------
// Compression request sizing (the /compress 400 investigation)
// ---------------------------------------------------------------------------

// The completion budget is counted against the SAME window as the prompt:
// llama.cpp's own overflow 400 ends "... 386429 of text input, 1748 of tool
// input, 16384 in the output". A max_tokens carried over from a bigger
// endpoint (the project settings hold 16384 from the local-model era) therefore
// pushes an otherwise-fitting request over a smaller provider's window.
TEST(compress_request_max_tokens_fits_the_window_it_is_sent_to) {
    std::string req;
    const std::string classify = R"({"summary":"s","classification":[{"turns":"1-2","tag":"core"},)"
                                 R"({"turns":"3-4","tag":"prune"}]})";
    json reply;
    reply["choices"] =
        json::array({json{{"message", json{{"role", "assistant"}, {"content", classify}}}}});
    int srv = spawn_mock_http(8953, req, "HTTP/1.1 200 OK", "application/json", reply.dump(),
                              /*connections=*/2); // classify, then the extract replay
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8953/v1";
    cfg.model = "kilo-auto/free";
    cfg.max_tokens = 16384;  // the stale project setting
    cfg.context_size = 4096; // what the selected provider's catalog reports
    agent::HttpLLMClient client(cfg);

    agent::Context ctx;
    ctx.push(msg("system", std::string(4000, 's'))); // ~1000 tokens
    for (int i = 0; i < 2; ++i) {
        ctx.push(msg("user", std::string(1600, 'u')));
        ctx.push(msg("assistant", std::string(1600, 'a')));
    }
    agent::CompressionConfig cc = agent::load_compression_config(cfg);
    auto compressor = agent::make_compressor();
    agent::CompressionResponse cr;
    compressor->compress(ctx, cc, client, nullptr, &cr);
    close(srv);

    const size_t sep = req.find("\r\n\r\n");
    ASSERT(sep != std::string::npos);
    json sent = json::parse(req.substr(sep + 4), nullptr, false);
    ASSERT(!sent.is_discarded());
    ASSERT_TRUE(sent.contains("max_tokens"));
    const size_t asked = sent["max_tokens"].get<size_t>();
    std::vector<agent::Message> on_the_wire;
    for (const auto& m : sent["messages"]) {
        agent::Message wire;
        wire.role = m.value("role", "");
        wire.content = m.value("content", "");
        on_the_wire.push_back(std::move(wire));
    }
    const size_t prompt = agent::estimate_tokens(on_the_wire);
    ASSERT(asked < cfg.max_tokens);                                  // clamped, not the stale ask
    ASSERT(asked + prompt <= static_cast<size_t>(cfg.context_size)); // fits the window it goes to
}

// The manual /compress path must size itself against the window the CURRENT
// provider reports, never a value a previous endpoint left in the config: the
// local server taught amber 262144, the gateway model has its own window, and
// the classify request is the largest request amber sends.
TEST(compress_uses_the_providers_window_not_a_stale_configured_one) {
    XdgGuard xdg("compress_window");
    agent::Workspace::set_root(".");
    agent::Config cfg;
    cfg.stream = false;
    cfg.context_size = 262144; // auto-resolved from the local endpoint
    cfg.context_explicit = false;
    agent::ToolRegistry reg;
    const auto cc = agent::load_compression_config(cfg);
    auto factory = [](const agent::Config&) -> std::unique_ptr<agent::LLMClient> {
        return std::make_unique<agent_test::FakeLLMClient>();
    };
    agent::Agent ag(cfg, reg, {}, agent::make_compressor(), agent::make_compression_gate(cc), {},
                    {}, nullptr, factory);
    agent::ModelCapabilities kilo_caps;
    kilo_caps.context = 131072;
    ag.set_model("kilo-auto/free", kilo_caps); // the selected provider's catalogue window

    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    msgs.push_back(std::move(sys));
    for (int i = 0; i < 6; ++i) {
        agent::Message u;
        u.role = "user";
        u.content = "q" + std::to_string(i);
        msgs.push_back(std::move(u));
        agent::Message a;
        a.role = "assistant";
        a.content = "a" + std::to_string(i);
        msgs.push_back(std::move(a));
    }
    ag.set_context(std::move(msgs));
    ag.compress_now();

    const auto live = ag.config_snapshot();
    ASSERT_TRUE(live != nullptr);
    ASSERT_EQ(live->context_size, 131072);
}

TEST(compression_gate_respects_threshold_setter) {
    agent::CompressionConfig cc;
    cc.min_turns = 1;
    auto gate = agent::make_compression_gate(cc);
    gate->set_threshold(0.9);
    agent::Config cfg;
    cfg.context_size = 100000;
    cfg.turn_counter = 25;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 89000;
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 91000;
    ASSERT(gate->should_compress(ctx, cfg));
}

// ---------------------------------------------------------------------------
// Experience / memory tests
// ---------------------------------------------------------------------------

TEST(memory_store_upsert_and_retrieve) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.content = "project uses make";
    mem.tags = {"build", "make"};
    mem.evidence_count = 3;
    mem.promoted = true;
    store->upsert(mem);
    auto results = store->top_memories(10, "how to build");
    ASSERT(!results.empty());
    ASSERT(results[0].content == "project uses make");
}

// The store, not its callers, owns the no-edit rule: a same-name entry with
// different content is skipped, and the original survives.
TEST(store_skips_a_name_conflict) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Memory original;
    original.name = "build";
    original.content = "make build";
    original.evidence_count = 3;
    original.promoted = true;
    store->upsert(original);

    agent::Memory conflicting;
    conflicting.name = "build";
    conflicting.content = "cmake --build";
    conflicting.evidence_count = 3;
    conflicting.promoted = true;
    store->upsert(conflicting);

    auto all = store->all_memories();
    ASSERT_EQ(all.size(), size_t{1});
    ASSERT_EQ(all[0].content, std::string("make build"));
}

TEST(store_skips_a_skill_name_conflict) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Skill original;
    original.name = "run-tests";
    original.content = "run make test";
    original.trigger_phrase = "test";
    original.evidence_count = 5;
    original.promoted = true;
    store->upsert(original);

    agent::Skill conflicting;
    conflicting.name = "run-tests";
    conflicting.content = "run ctest";
    conflicting.trigger_phrase = "test";
    conflicting.evidence_count = 5;
    conflicting.promoted = true;
    store->upsert(conflicting);

    auto all = store->all_skills();
    ASSERT_EQ(all.size(), size_t{1});
    ASSERT_EQ(all[0].content, std::string("run make test"));
}

TEST(memory_store_top_k_limits) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    for (int i = 0; i < 5; ++i) {
        agent::Memory mem;
        mem.content = "memory " + std::to_string(i);
        mem.evidence_count = i;
        mem.promoted = true;
        store->upsert(mem);
    }
    auto results = store->top_memories(3, "");
    ASSERT(results.size() == 3u);
}

TEST(memory_store_skill_trigger) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Skill sk;
    sk.content = "run tests";
    sk.trigger_phrase = "test";
    sk.evidence_count = 5;
    sk.promoted = true;
    store->upsert(sk);
    auto results = store->top_skills(10, "run the tests");
    ASSERT(!results.empty());
    ASSERT(results[0].content == "run tests");
    auto no_match = store->top_skills(10, "build the project");
    ASSERT(no_match.empty());
}

TEST(skill_no_dead_fields_in_serialization) {
    agent::Skill sk;
    sk.name = "deploy-cmd";
    sk.content = "make deploy";
    sk.trigger_phrase = "deploy";
    sk.evidence_count = 5;
    sk.promoted = true;
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->upsert(sk);
    std::string path = "/tmp/amber_skill_no_dead.json";
    std::remove(path.c_str());
    ASSERT(store->save(path));
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string txt = ss.str();
    ASSERT(txt.find("steps") == std::string::npos);
    ASSERT(txt.find("expected_outcome") == std::string::npos);
}

TEST(skill_legacy_json_loads_without_dead_fields) {
    std::string path = "/tmp/amber_skill_legacy.json";
    {
        std::ofstream f(path);
        f << R"({"version":1,"memories":[],"skills":[{"id":"1","name":"deploy","content":"make deploy","trigger_phrase":"deploy","steps":["a","b"],"expected_outcome":"done","tags":["deploy"],"evidence":3,"last_confirm_turn":0,"score":0,"promoted":true}]})";
    }
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    ASSERT(store->load(path));
    auto skills = store->top_skills(10, "deploy");
    ASSERT(skills.size() == 1u);
    ASSERT(skills[0].content == "make deploy");
    ASSERT(skills[0].trigger_phrase == "deploy");
}

TEST(experience_disabled_makes_no_store) {
    agent::ExperienceConfig ec;
    ec.enabled = false;
    ASSERT(agent::make_memory_store(ec) == nullptr);
}

TEST(experience_disabled_config_makes_no_store) {
    agent::Config cfg;
    cfg.experience_enabled = false;
    cfg.experience_store_path = "/tmp/amber_disabled_experience.json";
    auto ec = agent::load_experience_config(cfg);
    ASSERT_FALSE(ec.enabled);
    ASSERT(agent::make_memory_store(ec) == nullptr);
}

TEST(experience_store_project_default) {
    agent::Workspace::set_root("/tmp/amber_sk2_ws");
    agent::Config cfg;
    auto ec = agent::load_experience_config(cfg);
    // Per-project STATE, not the project tree: what one project taught the
    // agent must not travel with a checkout (or into a repository).
    ASSERT_EQ(ec.store_path, agent::Workspace::state_dir() + "/experience.json");
    ASSERT(ec.store_path.find("/.amber/") == std::string::npos);
}

TEST(experience_store_legacy_seed_once) {
    agent::Workspace::set_root("/tmp/amber_sk2_ws2");
    std::string legacy_dir = "/tmp/amber_sk2_home/.amber";
    std::string legacy = legacy_dir + "/memories.json";
    run_cmd("rm -rf /tmp/amber_sk2_home /tmp/amber_sk2_ws2");
    run_cmd("mkdir -p " + legacy_dir);
    {
        std::ofstream f(legacy);
        f << R"({"version":1,"memories":[{"id":"m1","name":"proj","content":"uses make","tags":[],"evidence":3,"last_confirm_turn":0,"score":0,"promoted":true}],"skills":[]})";
    }
    // A store written by the pre-state-layout amber, in the project tree: it
    // must be adopted too, not only the older ~/.amber file.
    run_cmd("mkdir -p /tmp/amber_sk2_ws2/.amber");
    {
        std::ofstream f("/tmp/amber_sk2_ws2/.amber/experience.json");
        f << R"({"version":1,"memories":[{"id":"m2","name":"proj","content":"from the project tree","tags":[],"evidence":3,"last_confirm_turn":0,"score":0,"promoted":true}],"skills":[]})";
    }
    setenv("HOME", "/tmp/amber_sk2_home", 1);

    agent::Config cfg;
    auto ec = agent::load_experience_config(cfg);
    ASSERT_EQ(ec.store_path, agent::Workspace::state_dir() + "/experience.json");
    {
        std::ifstream f(ec.store_path);
        std::stringstream ss;
        ss << f.rdbuf();
        // The project-tree store is the more recent one, so it is what the
        // adoption leaves in place.
        ASSERT(ss.str().find("from the project tree") != std::string::npos);
    }
    {
        std::ifstream f(legacy);
        std::stringstream ss;
        ss << f.rdbuf();
        ASSERT(ss.str().find("uses make") != std::string::npos);
    }
    {
        std::ofstream f(legacy);
        f << "changed-after-seed";
    }
    auto ec2 = agent::load_experience_config(cfg);
    {
        std::ifstream f(ec2.store_path);
        std::stringstream ss;
        ss << f.rdbuf();
        ASSERT(ss.str().find("from the project tree") != std::string::npos);
    }
}

TEST(memory_store_decay) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.content = "will decay";
    mem.evidence_count = 3;
    mem.promoted = true;
    store->upsert(mem);
    store->set_current_turn(2); // a later cycle: the turn-1 confirmation is old
    store->decay_all();
    auto results = store->top_memories(10, "");
    ASSERT(!results.empty());
    ASSERT(results[0].evidence_count == 2);
}

TEST(memory_retriever_empty) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::MemoryRetriever retriever(*store);
    auto suffix = retriever.build_block();
    ASSERT(suffix.empty());
}

TEST(memory_retriever_with_memories) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.content = "build system is make";
    mem.tags = {"build"};
    mem.evidence_count = 3;
    mem.promoted = true;
    store->upsert(mem);
    agent::MemoryRetriever retriever(*store);
    auto suffix = retriever.build_block();
    ASSERT(!suffix.empty());
    ASSERT(suffix.find("build system") != std::string::npos);
}

// The injected block is the stable prefix [system][memory], so it must be a
// function of the store alone. Selection by the current question reordered it
// on every turn, which is exactly a prefix change.
TEST(memory_block_is_a_function_of_the_store) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory build;
    build.name = "build";
    build.content = "the build system is make";
    build.tags = {"build"};
    build.evidence_count = 3;
    build.promoted = true;
    store->upsert(build);
    agent::Memory deploy;
    deploy.name = "deploy";
    deploy.content = "deployment uses ansible";
    deploy.tags = {"deploy"};
    deploy.evidence_count = 3;
    deploy.promoted = true;
    store->upsert(deploy);

    agent::MemoryRetriever retriever(*store);
    auto build_block = retriever.build_block();
    auto deploy_block = retriever.build_block();
    ASSERT_EQ(build_block, deploy_block);
}

// The cache is keyed by the store version: a turn-counter change (which moves
// freshness but not knowledge) must not re-render the prefix. A real upsert
// must.
TEST(memory_block_survives_a_turn_counter_change) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.name = "build";
    mem.content = "the build system is make";
    mem.evidence_count = 3;
    mem.promoted = true;
    store->upsert(mem);

    agent::MemoryRetriever retriever(*store, ec);
    const std::string first = retriever.build_block();
    store->set_current_turn(100);
    ASSERT_EQ(retriever.build_block(), first);

    agent::Memory extra;
    extra.name = "tests";
    extra.content = "run make test";
    extra.evidence_count = 2;
    extra.promoted = true;
    store->upsert(extra);
    ASSERT(retriever.build_block() != first);
}

TEST(memory_block_honours_the_configured_cap) {
    agent::ExperienceConfig ec;
    ec.max_memories = 1;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory high;
    high.name = "high";
    high.content = "highest evidence memory";
    high.evidence_count = 5;
    high.promoted = true;
    store->upsert(high);
    agent::Memory low;
    low.name = "low";
    low.content = "lower evidence memory";
    low.evidence_count = 2;
    low.promoted = true;
    store->upsert(low);

    agent::MemoryRetriever retriever(*store, ec);
    const std::string block = retriever.build_block();
    ASSERT(block.find("highest evidence memory") != std::string::npos);
    ASSERT(block.find("lower evidence memory") == std::string::npos);
}

TEST(memory_block_honours_the_token_budget) {
    agent::ExperienceConfig ec;
    ec.max_prompt_tokens = 30; // ~120 chars: the header plus one entry only
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    for (int i = 0; i < 4; ++i) {
        agent::Memory mem;
        mem.name = "m" + std::to_string(i);
        mem.content = "memory " + std::to_string(i) + " " + std::string(40, 'x');
        mem.evidence_count = 10 - i;
        mem.promoted = true;
        store->upsert(mem);
    }
    agent::MemoryRetriever retriever(*store, ec);
    const std::string block = retriever.build_block();
    ASSERT(block.find("memory 0") != std::string::npos);
    ASSERT(block.find("memory 3") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Integration: compression pipeline + memory end-to-end
// ---------------------------------------------------------------------------

TEST(integration_apply_and_retrieve) {
    // Simulate a realistic conversation with a CompressionResponse from
    // the LLM, then verify the apply+retrieve pipeline end-to-end.

    std::vector<agent::Message> hist = {
        msg("system", "You are amber, a helpful coding assistant."),
        msg("user", "How is this project built?"),
        msg("assistant", "Let me check the build system."),
        msg("tool", "GNUmakefile\nconfigure script\n", "read"),
        msg("assistant", "This project uses GNU make with ./configure."),
        msg("user", "Run the tests for me."),
        msg("assistant", "Running the test suite now."),
        msg("tool",
            "compressor_test.cpp: OK\nmemory_store_test.cpp: OK\n"
            "103 passed, 0 failed\n",
            "bash"),
        msg("assistant", "All 103 tests passed."),
        msg("user", "What was the test result again?"),
    };

    // Phase 1: Apply a pretend LLM classification response.
    //   - system + last user = core
    //   - tool results + old assistant = prune
    //   - everything else = context with summary
    agent::CompressionResponse cr;
    cr.segments.push_back({0, 0, agent::Classification::core, ""});
    cr.segments.push_back(
        {1, 2, agent::Classification::context, "user asked about build system, assistant checked"});
    cr.segments.push_back({3, 3, agent::Classification::prune, ""});
    cr.segments.push_back(
        {4, 4, agent::Classification::context, "assistant answered build system question"});
    cr.segments.push_back({5, 5, agent::Classification::core, ""});
    cr.segments.push_back({6, 7, agent::Classification::prune, ""});
    cr.segments.push_back({8, 8, agent::Classification::prune, ""});
    cr.segments.push_back({9, 9, agent::Classification::core, ""});

    auto compressed = agent::apply_classification(hist, cr);
    ASSERT(!compressed.empty());
    ASSERT(compressed.size() < hist.size());

    // Phase 2: Extract memories by feeding ops to the store.
    // Simulate 3 compression cycles confirming the same knowledge
    // to reach the promotion threshold (3) and become retrievable.
    agent::ExperienceConfig ec;
    ec.enabled = true;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);

    cr.memory_ops.push_back({"build system",
                             "project uses GNU make with ./configure",
                             {"build", "make"},
                             "upsert",
                             ""});
    cr.memory_ops.push_back(
        {"test results", "103 tests passed", {"tests", "testing"}, "upsert", ""});

    // Single compression cycle — memories are now promoted immediately
    // (evidence=3, promoted=true) since the LLM confirmed them.
    store->set_current_turn(1);
    agent::apply_memory_ops(*store, cr.memory_ops, "");

    // Phase 3: Retrieve relevant memories
    agent::MemoryRetriever retriever(*store);
    std::string suffix = retriever.build_block();
    ASSERT(!suffix.empty());
    bool found = suffix.find("GNU make") != std::string::npos ||
                 suffix.find("./configure") != std::string::npos;
    ASSERT(found);

    // Phase 4: Verify decay — evidence 3 → 2 after one later cycle
    store->set_current_turn(2);
    store->decay_all();
    auto after_decay = store->top_memories(10, "build");
    ASSERT(!after_decay.empty());
    ASSERT(after_decay[0].evidence_count == 2);
}

// ---------------------------------------------------------------------------
// Agent memory extraction (FIX-002)
// ---------------------------------------------------------------------------

TEST(deprecate_op_reaches_the_store_by_name) {
    // The extract step documents deprecation by name. A name-only op was
    // filtered out by the parser (empty content) and the store only matched by
    // content, so the path was unreachable at both ends.
    auto cr = agent::parse_compression_response(
        R"({"memories":[{"name":"make-fact","action":"deprecate"}]})");
    ASSERT_EQ(cr.memory_ops.size(), size_t{1});
    ASSERT_EQ(cr.memory_ops[0].action, std::string("deprecate"));

    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Memory mem;
    mem.name = "make-fact";
    mem.content = "project uses make";
    mem.evidence_count = 2;
    mem.promoted = true;
    store->upsert(mem);
    agent::apply_memory_ops(*store, cr.memory_ops, "");
    auto all = store->all_memories();
    ASSERT_EQ(all.size(), size_t{1});
    ASSERT_EQ(all[0].evidence_count, 1);
}

TEST(apply_memory_ops_deprecate_decreases_evidence) {
    // A "deprecate" op must actually lower evidence — it previously decremented
    // then upserted, and the store's upsert always increments: net zero (or
    // worse, +1). Deprecated knowledge must lose weight.
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.name = "make-fact";
    mem.content = "project uses make";
    mem.tags = {"build"};
    mem.evidence_count = 3;
    mem.promoted = true;
    store->upsert(mem);
    agent::KnowledgeOp op;
    op.name = "make-fact";
    op.content = "project uses make";
    op.action = "deprecate";
    agent::apply_memory_ops(*store, {op}, "");
    auto all = store->all_memories();
    ASSERT_EQ(all.size(), size_t{1});
    ASSERT_EQ(all[0].evidence_count, 2);
}

TEST(apply_memory_ops_deprecate_removes_at_zero_evidence) {
    // Deprecating the last evidence point removes the item from the store.
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Memory mem;
    mem.name = "one-hit";
    mem.content = "one-time fact";
    mem.evidence_count = 1;
    mem.promoted = true;
    store->upsert(mem);
    agent::KnowledgeOp op;
    op.name = "one-hit";
    op.content = "one-time fact";
    op.action = "deprecate";
    agent::apply_memory_ops(*store, {op}, "");
    ASSERT(store->all_memories().empty());
}

TEST(apply_skill_ops_deprecate_decreases_evidence) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    store->set_current_turn(1);
    agent::Skill sk;
    sk.name = "run-tests";
    sk.content = "Run 'make test' in workspace root";
    sk.trigger_phrase = "test";
    sk.evidence_count = 5;
    sk.promoted = true;
    store->upsert(sk);
    agent::KnowledgeOp op;
    op.name = "run-tests";
    op.content = "Run 'make test' in workspace root";
    op.action = "deprecate";
    agent::apply_skill_ops(*store, {op}, "");
    auto all = store->all_skills();
    ASSERT_EQ(all.size(), size_t{1});
    ASSERT_EQ(all[0].evidence_count, 4);
}

// ---------------------------------------------------------------------------
// Headroom enforcement
// ---------------------------------------------------------------------------

TEST(headroom_budget_is_below_the_trigger_threshold) {
    // A compaction must not finish in a state that satisfies the trigger again
    // the moment the cooldown lapses. The headroom budget is derived from the
    // trigger, not a fixed 75% that sits above a 70% threshold.
    std::vector<agent::Message> hist = {msg("system", "Amber")};
    for (int i = 0; i < 5; ++i)
        hist.push_back(msg("user", std::string(500, 'a') + std::to_string(i)));
    hist.push_back(msg("user", "active task"));
    hist.push_back(compressed_ctx_msg());

    agent::CompressionConfig cfg;
    cfg.context_size = 1000;
    cfg.threshold = 0.7;
    cfg.keep_last_prompts = 1;
    // ~665 tokens: below the old fixed 75% budget (750, no trim) and above the
    // derived 60% budget (600, trim).
    ASSERT(agent::estimate_tokens(hist) > 600u);
    ASSERT(agent::estimate_tokens(hist) < 750u);
    auto out = agent::enforce_headroom(hist, 1000, cfg);
    ASSERT(agent::estimate_tokens(out) <= 600u);
}

TEST(enforce_headroom_archives_oldest_instead_of_placeholders) {
    // Over-budget messages were replaced with fake "[over-budget archived]"
    // system messages that polluted the prompt. They must instead be dropped
    // and recorded as archive entries on the compressed-context message.
    agent::Message ctx_msg;
    ctx_msg.role = "system";
    ctx_msg.content = "Compressed conversation context:\n{\"archive\":[],\"facts\":{}}";
    std::vector<agent::Message> msgs = {
        msg("system", "Amber"),
        msg("user", std::string(4000, 'a')),
        msg("assistant", std::string(4000, 'b')),
        msg("user", std::string(4000, 'c')),
        ctx_msg,
    };
    // The derived headroom budget is 60% of 3000 = 1800 tokens; the three
    // 4000-char messages (~1004 tokens each) put us at ~3033, so the two oldest
    // go. keep_last_prompts=1 protects only the active prompt.
    agent::CompressionConfig cfg;
    cfg.context_size = 3000;
    cfg.target_pct = 10;
    cfg.keep_last_prompts = 1;
    auto out = agent::enforce_headroom(std::move(msgs), 3000, cfg);
    ASSERT(agent::estimate_tokens(out) <= 1800u);
    ASSERT_EQ(out[1].content, std::string(4000, 'c')); // the active prompt survives
    for (const auto& m : out)
        ASSERT(m.content.find("[over-budget archived]") == std::string::npos);
    bool archived = false;
    for (const auto& m : out)
        if (m.content.find("Compressed conversation context:") == 0)
            archived = m.content.find("archive") != std::string::npos &&
                       m.content.find("over-budget") != std::string::npos;
    ASSERT(archived);
}

TEST(compression_preserves_tool_pairing_across_budget_sweep) {
    // Every budget must yield a provider-valid history. A cut that lands between
    // an assistant tool_calls message and its result used to leave an orphan
    // (or a dangling call) because the repair runs before the budget passes.
    for (int window = 300; window <= 4200; window += 13) {
        std::vector<agent::Message> hist = {msg("system", "Amber")};
        for (int r = 0; r < 8; ++r) {
            const std::string id = "c" + std::to_string(r);
            hist.push_back(msg("user", "task " + std::to_string(r) + ": " + std::string(80, 'u')));
            hist.push_back(tool_call_msg(id, "read"));
            hist.push_back(tool_result_msg(id, std::string(400, 'x')));
        }
        hist.push_back(msg("user", "the active task"));
        hist.push_back(compressed_ctx_msg());

        agent::CompressionConfig cfg;
        cfg.target_pct = 100; // target == window, so the sweep moves the cut
        cfg.keep_last_prompts = 1;
        auto out = agent::enforce_target_budget(hist, static_cast<size_t>(window), cfg);
        ASSERT_EQ(orphan_tool_results(out), 0u);
        ASSERT_EQ(dangling_tool_calls(out), 0u);
        ASSERT(has_real_user(out));
    }
}

TEST(compression_never_drops_the_active_user_message) {
    // The protected tail can exceed the headroom budget on its own. Dropping it
    // leaves a history with no task at all — the minimum-context invariant is a
    // post-condition of the pipeline, not of one intermediate step.
    std::vector<agent::Message> hist = {
        msg("system", "Amber"),
        msg("user", std::string(4000, 'a')),
        msg("assistant", std::string(4000, 'b')),
        compressed_ctx_msg(),
        msg("user", "THE ACTIVE TASK: finish the refactor " + std::string(4000, 't')),
    };
    agent::CompressionConfig cfg;
    cfg.context_size = 900;
    cfg.target_pct = 10;
    cfg.keep_last_prompts = 10;
    auto out = agent::enforce_headroom(std::move(hist), 900, cfg); // budget 675 < the tail
    bool active = false;
    for (const auto& m : out)
        if (m.content.find("THE ACTIVE TASK") != std::string::npos)
            active = true;
    ASSERT(active);
    ASSERT(has_real_user(out));
}

TEST(compression_budget_is_measured_after_the_archive_is_written) {
    // The archive is grown by the pass that trims to the budget. Measured before
    // the write, the post-condition was optimistic: 24 drops added ~480 tokens
    // of bookkeeping on top of a budget that had just been met.
    std::vector<agent::Message> hist = {msg("system", "Amber")};
    for (int r = 0; r < 40; ++r) {
        const std::string id = "c" + std::to_string(r);
        hist.push_back(tool_call_msg(id, "read"));
        hist.push_back(tool_result_msg(id, std::string(200, 'x')));
    }
    hist.push_back(msg("user", "the active task"));
    hist.push_back(compressed_ctx_msg());

    agent::CompressionConfig cfg;
    cfg.target_pct = 100;
    cfg.keep_last_prompts = 1;
    const size_t budget = 1600;
    auto out = agent::enforce_target_budget(hist, budget, cfg);
    ASSERT(agent::estimate_tokens(out) <= budget);
}

TEST(protected_tail_is_bounded_by_tokens_not_prompt_count) {
    // Three turns of ~1000 tokens each. A 4096-token window with a 10% target
    // gives the protected tail ~409 tokens, so only the final turn fits; the
    // prompt-count rule (10) protected all three and made the pass a no-op.
    std::vector<agent::Message> hist = {msg("system", "Amber")};
    for (int t = 0; t < 3; ++t) {
        const std::string id = "c" + std::to_string(t);
        hist.push_back(msg("user", "task " + std::to_string(t)));
        hist.push_back(tool_call_msg(id, "read"));
        hist.push_back(tool_result_msg(id, std::string(4000, 'x')));
    }
    hist.push_back(msg("user", "the active task"));
    hist.push_back(compressed_ctx_msg());

    agent::CompressionConfig cfg;
    cfg.context_size = 4096;
    cfg.target_pct = 10;        // target 409 tokens
    cfg.keep_last_prompts = 10; // the old rule protected every turn

    const std::size_t before = hist.size();
    auto out = agent::enforce_target_budget(hist, cfg.context_size, cfg);
    ASSERT(out.size() < before);
    bool active = false;
    for (const auto& m : out)
        if (m.content == "the active task")
            active = true;
    ASSERT(active);
}

TEST(enforce_target_budget_archives_oldest_and_keeps_the_active_task) {
    agent::Message ctx_msg;
    ctx_msg.role = "system";
    ctx_msg.content = "Compressed conversation context:\n{\"archive\":[],\"facts\":{}}";
    std::vector<agent::Message> msgs = {
        msg("system", "Amber"),
        msg("user", std::string(4000, 'a')),      // archivable
        msg("assistant", std::string(4000, 'b')), // archivable
        msg("user", std::string(4000, 'c')),      // the active task: protected
        ctx_msg,
    };
    agent::CompressionConfig cfg;
    cfg.target_pct = 10;       // 10% of 20000 = 2000 tokens
    cfg.keep_last_prompts = 1; // protect the last real user prompt
    // Three ~1004-token messages put us at ~3012 against a 2000 budget, so the
    // two oldest non-system messages go and the protected tail survives.
    auto out = agent::enforce_target_budget(std::move(msgs), 20000, cfg);
    ASSERT_EQ(out.size(), size_t{3});
    ASSERT_EQ(out[0].role, "system");
    ASSERT_EQ(out[1].content, std::string(4000, 'c'));
    // Each dropped turn is recorded in the archive block rather than silently
    // vanishing.
    ASSERT(out[2].content.find("compressed to meet target budget") != std::string::npos);

    // Without a compressed-context message there is nothing to record into, so
    // the pass bails instead of dropping messages it cannot account for.
    std::vector<agent::Message> no_ctx = {
        msg("system", "Amber"),
        msg("user", std::string(4000, 'a')),
        msg("user", std::string(4000, 'b')),
    };
    auto same = agent::enforce_target_budget(std::move(no_ctx), 4000, cfg); // target 400
    ASSERT_EQ(same.size(), size_t{3});

    // An unknown context window never trims.
    std::vector<agent::Message> unknown = {msg("system", "Amber"), msg("user", "hi")};
    ASSERT_EQ(agent::enforce_target_budget(std::move(unknown), 0, cfg).size(), size_t{2});
}

TEST(agent_extract_memories_from_tool_results) {
    // Verify MemoryStore round-trip (the core store behavior, not the
    // removed heuristic extraction). The LLM-based extraction in
    // compress_now() is the correct knowledge extraction path.
    agent::ExperienceConfig ec;
    ec.enabled = true;
    auto store = agent::make_memory_store(ec);

    store->set_current_turn(1);
    agent::Memory mem;
    mem.content = "project uses GNU make with ./configure";
    mem.tags = {"build"};
    mem.evidence_count = 3;
    store->upsert(mem);

    // Second upsert promotes (evidence 3→4, threshold is 3)
    store->upsert(mem);

    auto top = store->top_memories(10, "build");
    ASSERT(!top.empty());
}

// ---------------------------------------------------------------------------
// Turn: the loop's state machine (issue #276)
//
// These are the transition rules themselves, not the loop's end-to-end effect.
// The loop suite above still pins the behaviour; these pin the state that
// produces it, so a bound can be changed without re-deriving it from a fake
// model's transcript.
// ---------------------------------------------------------------------------

namespace {
agent::Turn fresh_turn(int max_wall_ms = 0) {
    return agent::Turn(std::chrono::steady_clock::now(), max_wall_ms);
}
} // namespace

TEST(turn_silence_stops_at_the_bound_and_tool_work_resets_it) {
    agent::Turn turn = fresh_turn();
    ASSERT_FALSE(turn.record_silent_reply());
    ASSERT_TRUE(turn.record_silent_reply());

    agent::Turn reset = fresh_turn();
    ASSERT_FALSE(reset.record_silent_reply());
    reset.record_tool_reply();
    ASSERT_FALSE(reset.record_silent_reply());
}

TEST(turn_notice_patience_is_separate_from_silence) {
    agent::Turn turn = fresh_turn();
    ASSERT_FALSE(turn.record_notice());
    ASSERT_FALSE(turn.record_notice());
    ASSERT_TRUE(turn.record_notice());

    // A silent reply and a notice are different signals: one of each is not
    // "two in a row" of either.
    agent::Turn mixed = fresh_turn();
    ASSERT_FALSE(mixed.record_silent_reply());
    ASSERT_FALSE(mixed.record_notice());
    ASSERT_FALSE(mixed.record_silent_reply());
}

TEST(turn_repeated_call_is_reported_once_per_milestone) {
    agent::Turn turn = fresh_turn();
    ASSERT_EQ(turn.observe_repeated_call("read|a"), 0);
    ASSERT_EQ(turn.note_repeated_call(), 0);
    ASSERT_EQ(turn.observe_repeated_call("read|a"), 1);
    ASSERT_EQ(turn.note_repeated_call(), 0);
    ASSERT_EQ(turn.observe_repeated_call("read|a"), 2);
    // The note names the run length, and it is said once at that milestone.
    ASSERT_EQ(turn.note_repeated_call(), 3);
    ASSERT_EQ(turn.note_repeated_call(), 0);
    // A different call starts a new run.
    ASSERT_EQ(turn.observe_repeated_call("read|b"), 0);
}

TEST(turn_dispatch_loop_stops_at_the_guard_limit) {
    agent::Turn turn = fresh_turn();
    // The counter counts REPEATS, so the first batch sets the fingerprint and
    // the fourth identical batch is the third repeat.
    ASSERT_FALSE(turn.record_dispatch_fingerprint("fp"));
    ASSERT_FALSE(turn.record_dispatch_fingerprint("fp"));
    ASSERT_FALSE(turn.record_dispatch_fingerprint("fp"));
    ASSERT_TRUE(turn.record_dispatch_fingerprint("fp"));
    ASSERT_FALSE(turn.record_dispatch_fingerprint("other"));
}

TEST(turn_text_repeat_steers_then_stops) {
    agent::Turn turn = fresh_turn();
    // The first reply is the baseline, so the steer lands on the third
    // identical arrival and the stop on the sixth.
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::None);
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::None);
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::Steer);
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::None);
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::None);
    ASSERT_TRUE(turn.observe_text("x") == agent::TextRepeat::Unrecoverable);
    // Different text is a new baseline, not a repeat.
    ASSERT_TRUE(turn.observe_text("y") == agent::TextRepeat::None);
    ASSERT_TRUE(turn.observe_text("y") == agent::TextRepeat::None);
}

TEST(turn_wall_clock_budget_exhausts_at_the_deadline) {
    const auto start = std::chrono::steady_clock::now() - std::chrono::milliseconds(1500);
    agent::Turn turn(start, 1000);
    ASSERT_TRUE(turn.wall_clock_exceeded(std::chrono::steady_clock::now()));
    ASSERT_TRUE(turn.elapsed_ms(std::chrono::steady_clock::now()) >= 1500);

    agent::Turn unlimited = fresh_turn();
    ASSERT_FALSE(unlimited.wall_clock_exceeded(std::chrono::steady_clock::now()));
}

TEST(turn_budget_notes_are_said_once) {
    agent::Turn turn = fresh_turn(1000);
    ASSERT_FALSE(turn.note_iteration_budget(1, 10)); // 20% spent
    ASSERT_TRUE(turn.note_iteration_budget(8, 10));  // 90% spent
    ASSERT_FALSE(turn.note_iteration_budget(9, 10)); // said already

    agent::Turn wall(std::chrono::steady_clock::now(), 1000);
    ASSERT_FALSE(wall.note_wall_budget(1000, std::chrono::steady_clock::now()));
    const auto late = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    ASSERT_TRUE(wall.note_wall_budget(1000, late));
    ASSERT_FALSE(wall.note_wall_budget(1000, late));
}

TEST(turn_internal_failures_stop_after_the_second) {
    agent::Turn turn = fresh_turn();
    ASSERT_FALSE(turn.record_internal_failure());
    ASSERT_TRUE(turn.record_internal_failure());
}

TEST(turn_recovery_steer_is_claimable_once) {
    agent::Turn turn = fresh_turn();
    ASSERT_TRUE(turn.claim_recovery_steer());
    ASSERT_FALSE(turn.claim_recovery_steer());
}

TEST(turn_records_how_the_turn_closed) {
    agent::Turn closed = fresh_turn();
    ASSERT_FALSE(closed.ran_tools());
    closed.record_tool_reply();
    ASSERT_TRUE(closed.ran_tools());
    closed.record_failure("provider 500");
    closed.close_with("the answer");
    ASSERT_TRUE(closed.closed_by_model());
    ASSERT_TRUE(closed.has_reply());
    ASSERT_EQ(closed.last_failure(), "provider 500");
    ASSERT_EQ(closed.take_reply(), "the answer");
    ASSERT_FALSE(closed.has_reply());

    // A bound-ended turn is not "closed by the model", and its reply is the
    // stop reason the host sees.
    agent::Turn stopped = fresh_turn();
    stopped.stop_with("[stopped: budget]");
    ASSERT_FALSE(stopped.closed_by_model());
    ASSERT_EQ(stopped.take_reply(), "[stopped: budget]");
}

// ---------------------------------------------------------------------------
// Agent run-loop behavioral spec (FIX-003)
// ---------------------------------------------------------------------------

TEST(agent_text_only_reply) {
    // Use an existing integration test pattern: verify agent history round-trips
    agent::Config cfg;
    cfg.max_tool_iterations = 1;
    agent::ToolRegistry reg;
    agent::Agent ag(cfg, reg);

    agent::Message sys;
    sys.role = "system";
    sys.content = "test";
    agent::Message u;
    u.role = "user";
    u.content = "say hello";
    agent::Message a;
    a.role = "assistant";
    a.content = "Hello world";
    ag.set_context({sys, u, a});

    ASSERT(ag.context().size() >= 3u);
    ASSERT(ag.context().get_all().back().role == "assistant");
    ASSERT(ag.context().get_all().back().content.find("Hello") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Compression observer (FIX-004)
// ---------------------------------------------------------------------------

TEST(compression_observer_interface) {
    // CompressionObserver must exist and be usable as a virtual base.
    // This test fails to COMPILE until the interface is declared in
    // compressor.h.  Once it exists, a simple implementation is created
    // to verify it's not abstract.
    struct Obs : agent::CompressionObserver {
        bool called = false;
        void on_compress_start(size_t, size_t) override { called = true; }
    };
    Obs o;
    o.on_compress_start(0, 0);
    ASSERT(o.called);
}

// ---------------------------------------------------------------------------
// Build system — separate core and tool archives (FIX-010)
// ---------------------------------------------------------------------------

TEST(build_system_separates_core_and_tools) {
    // After FIX-010, `make lib` produces separate archives:
    //   libagent_core.a  — lib/*.o (domain core only)
    //   libagent_tools.a — tools/*.o (tool adapters)
    // This test verifies both exist with the expected contents.
    // Currently (pre-fix) only libagent.a exists — the test fails.
    std::string core_archive = "libagent_core.a";
    std::string tools_archive = "libagent_tools.a";
    std::ifstream core_f(core_archive);
    std::ifstream tools_f(tools_archive);
    if (!core_f.is_open()) {
        std::fprintf(stderr, "%s not found - build system not split yet\n", core_archive.c_str());
        ASSERT(false);
    }
    if (!tools_f.is_open()) {
        std::fprintf(stderr, "%s not found - build system not split yet\n", tools_archive.c_str());
        ASSERT(false);
    }
    core_f.close();
    tools_f.close();

    // Verify core archive has no tool objects
    std::string core_contents;
    {
        std::array<char, 128> buf;
        auto deleter = [](FILE* f) {
            if (f)
                pclose(f);
        };
        std::unique_ptr<FILE, decltype(deleter)> pipe(popen(("ar t " + core_archive).c_str(), "r"),
                                                      deleter);
        if (pipe)
            while (fgets(buf.data(), buf.size(), pipe.get()))
                core_contents += buf.data();
    }
    if (core_contents.find("tools/") != std::string::npos) {
        std::fprintf(stderr, "core archive contains tool objects\n");
        ASSERT(false);
    }
}

// ---------------------------------------------------------------------------
// CancellationToken (FIX-001)
// ---------------------------------------------------------------------------

TEST(cancel_token_default_is_not_requested) {
    agent::CancellationToken t;
    ASSERT_FALSE(t.is_requested());
}

TEST(cancel_token_request_sets_flag) {
    agent::CancellationToken t;
    t.request();
    ASSERT_TRUE(t.is_requested());
}

TEST(cancel_token_clear_resets_flag) {
    agent::CancellationToken t;
    t.request();
    ASSERT_TRUE(t.is_requested());
    t.clear();
    ASSERT_FALSE(t.is_requested());
}

TEST(cancel_token_tokens_are_independent) {
    agent::CancellationToken t1, t2;
    t1.request();
    ASSERT_TRUE(t1.is_requested());
    ASSERT_FALSE(t2.is_requested());
    t2.request();
    ASSERT_TRUE(t1.is_requested());
    ASSERT_TRUE(t2.is_requested());
    t1.clear();
    ASSERT_FALSE(t1.is_requested());
    ASSERT_TRUE(t2.is_requested());
}

TEST(cancel_token_copies_share_state) {
    agent::CancellationToken t1;
    t1.request();
    agent::CancellationToken t2 = t1; // copy — same underlying state
    ASSERT_TRUE(t2.is_requested());
    t2.clear();
    ASSERT_FALSE(t1.is_requested()); // shared: clearing t2 clears t1
}

// ---------------------------------------------------------------------------
// Context immutable stack + event source
// ---------------------------------------------------------------------------

TEST(context_push_seals_message) {
    agent::Context ctx;
    agent::Message m;
    m.role = "user";
    m.content = "hello";
    m.tool_calls = agent::json::array({agent::json::object({{"id", "1"}})});

    ctx.push(std::move(m));
    // After push the original reference is moved-from; the message is sealed.
    ASSERT_EQ(ctx.size(), 1u);
    ASSERT_EQ(ctx.get_all().back().role, "user");
    ASSERT_EQ(ctx.get_all().back().content, "hello");

    // get_all() returns const& — compiler enforces read-only.
    const auto& ref = ctx.get_all();
    ASSERT_EQ(ref.back().content, "hello");
    // Attempting ref.back().content = "x" would fail at compile time.
}

TEST(context_token_count_tracks_content) {
    agent::Context ctx;
    ASSERT_EQ(ctx.token_count(), 0u);

    agent::Message m;
    m.role = "user";
    m.content = "hello world"; // 11 chars / 4 = 2 + 4 overhead = 6
    ctx.push(std::move(m));
    ASSERT_EQ(ctx.token_count(), 6u);

    agent::Message m2;
    m2.role = "assistant";
    m2.content = std::string(100, 'x'); // 100/4 = 25 + 4 = 29
    ctx.push(std::move(m2));
    ASSERT_EQ(ctx.token_count(), 35u); // 6 + 29

    ctx.clear();
    ASSERT_EQ(ctx.token_count(), 0u);
    ASSERT_EQ(ctx.size(), 0u);
}

TEST(context_lifo_pop) {
    agent::Context ctx;
    for (int i = 0; i < 5; ++i) {
        agent::Message m;
        m.role = "user";
        m.content = "msg" + std::to_string(i);
        ctx.push(std::move(m));
    }
    ASSERT_EQ(ctx.size(), 5u);

    // Pop is LIFO — removes the most recently pushed message.
    auto top = ctx.pop();
    ASSERT_EQ(top.content, "msg4");
    ASSERT_EQ(ctx.size(), 4u);

    top = ctx.pop();
    ASSERT_EQ(top.content, "msg3");
    ASSERT_EQ(ctx.size(), 3u);

    ctx.clear();
    ASSERT_EQ(ctx.size(), 0u);
    ASSERT_EQ(ctx.token_count(), 0u);
}

TEST(context_hash_chain_integrity) {
    // The hash chain asserts on every get_all(). If any mutation method
    // corrupts the chain (or if someone modifies a sealed message via
    // const_cast or a rogue method), get_all() crashes with assert failure.
    // This test exercises every mutation path to ensure the chain survives.

    agent::Context ctx;

    // Push the system prompt.
    agent::Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant.";
    ctx.push(std::move(sys));
    ASSERT_EQ(ctx.size(), 1u);
    // get_all() verifies the hash chain internally.
    ASSERT_EQ(ctx.get_all().size(), 1u);

    // Push user and assistant turns.
    agent::Message u1, a1, u2, a2;
    u1.role = "user";
    u1.content = "hello";
    a1.role = "assistant";
    a1.content = "hi there";
    u2.role = "user";
    u2.content = "what is c++";
    a2.role = "assistant";
    a2.content = "a language";
    ctx.push(std::move(u1));
    ctx.get_all();
    ctx.push(std::move(a1));
    ctx.get_all();
    ctx.push(std::move(u2));
    ctx.get_all();
    ctx.push(std::move(a2));
    ctx.get_all();
    ASSERT_EQ(ctx.size(), 5u);

    // Pop the last assistant reply (LIFO).
    auto popped = ctx.pop();
    ASSERT_EQ(popped.content, "a language");
    ctx.get_all(); // chain must survive pop
    ASSERT_EQ(ctx.size(), 4u);

    // Pop again.
    popped = ctx.pop();
    ASSERT_EQ(popped.content, "what is c++");
    ctx.get_all(); // chain must survive second pop
    ASSERT_EQ(ctx.size(), 3u);

    // Clear and re-push from scratch.
    ctx.clear();
    ctx.get_all(); // chain must survive clear
    ASSERT_EQ(ctx.size(), 0u);

    agent::Message m;
    m.role = "user";
    m.content = "fresh start";
    ctx.push(std::move(m));
    ctx.get_all(); // chain must survive rebuild
    ASSERT_EQ(ctx.size(), 1u);
    ASSERT_EQ(ctx.get_all().back().content, "fresh start");
}

TEST(context_event_source_delivers_to_all_subscribers) {
    agent::Context ctx;
    agent::ContextEventSource src;

    int sub1_count = 0, sub2_count = 0;
    size_t sub1_tokens = 0, sub2_tokens = 0;

    src.subscribe([&](size_t t, size_t) {
        ++sub1_count;
        sub1_tokens = t;
    });
    src.subscribe([&](size_t t, size_t) {
        ++sub2_count;
        sub2_tokens = t;
    });

    // Publish initial event.
    src.publish(ctx.token_count(), ctx.size());
    ASSERT_EQ(sub1_count, 1);
    ASSERT_EQ(sub2_count, 1);

    // Push a message and publish again.
    agent::Message m;
    m.role = "user";
    m.content = "test";
    ctx.push(std::move(m));
    src.publish(ctx.token_count(), ctx.size());

    ASSERT_EQ(sub1_count, 2);
    ASSERT_EQ(sub2_count, 2);
    ASSERT(sub1_tokens > 0u);
    ASSERT_EQ(sub1_tokens, sub2_tokens);
}

TEST(context_event_integration_with_agent) {
    // Verify that Agent's context mutations fire events to subscribers
    // without requiring a live LLM server.
    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    agent::ToolRegistry reg;
    agent::Agent ag(cfg, reg);

    int events_fired = 0;
    size_t last_tokens = 0;
    size_t last_msgs = 0;
    ag.context_events().subscribe([&](size_t t, size_t m) {
        ++events_fired;
        last_tokens = t;
        last_msgs = m;
    });

    // Initially empty — no events yet (system prompt is pushed in run()).
    ASSERT_EQ(events_fired, 0);

    // set_context fires one bulk event after clearing + pushing.
    agent::Message msg;
    msg.role = "user";
    msg.content = "test";
    ag.set_context({msg});
    ASSERT_EQ(events_fired, 1);
    ASSERT(last_tokens > 0u);
    ASSERT_EQ(last_msgs, 1u);

    // Replace with two messages: one bulk event.
    agent::Message msg2;
    msg2.role = "assistant";
    msg2.content = "reply";
    ag.set_context({msg, msg2});
    ASSERT_EQ(events_fired, 2);
    ASSERT_EQ(last_msgs, 2u);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    return agent::test::run_all(argc, argv);
}

// ---------------------------------------------------------------------------
// Request repairs (#275)
//
// Each strategy is exercised against a fake host, so the conditions and the
// messages a repair produces are pinned without running the loop.
// ---------------------------------------------------------------------------

namespace {

class FakeRepairHost final : public agent::RepairHost {
public:
    std::string model() const override { return configured_model; }

    void report_error(const std::string& kind, const std::string& message) override {
        errors.push_back(kind + ": " + message);
    }

    void status(const std::string& line) override { statuses.push_back(line); }

    void announce(const std::string& note) override { notes.push_back(note); }

    std::vector<agent::ModelInfo> server_models() override { return models; }

    void adopt_model(const std::string& id, const agent::ModelCapabilities& caps) override {
        adopted_model = id;
        adopted_caps = caps;
    }

    std::string request_api_key(const char* stage) override {
        stages.emplace_back(stage);
        return key;
    }

    void adopt_api_key(const std::string& value) override { adopted_key = value; }

    std::string configured_model = "local/current";
    std::vector<agent::ModelInfo> models;
    std::string key;
    std::string adopted_model;
    agent::ModelCapabilities adopted_caps;
    std::string adopted_key;
    std::vector<std::string> errors;
    std::vector<std::string> statuses;
    std::vector<std::string> notes;
    std::vector<std::string> stages;
};

// A strategy with a fixed answer, to prove the ladder walks in order.
class StubRepair final : public agent::RepairStrategy {
public:
    StubRepair(agent::RequestFailure kind, agent::RetryRequest answer)
        : kind_(kind), answer_(answer) {}

    agent::RequestFailure handles() const noexcept override { return kind_; }

    agent::RetryRequest apply(const std::string&, const std::string&, bool,
                              agent::RepairHost&) const override {
        return answer_;
    }

private:
    agent::RequestFailure kind_;
    agent::RetryRequest answer_;
};

const char* const kTemplateParserError = "Unable to generate parser for this template";
const char* const kModelNameError = "model 'x' does not exist";
const char* const kAuthError = "HTTP 401 Unauthorized";

} // namespace

TEST(repair_template_parser_drops_tools_and_says_so) {
    FakeRepairHost host;
    const auto repair = agent::make_template_parser_repair();
    ASSERT_TRUE(agent::RetryRequest::WithoutTools ==
                repair->apply(kTemplateParserError, "generation", true, host));
    ASSERT_EQ(host.errors.size(), 1u);
    ASSERT(host.errors[0].find("template_parser") == 0);
    ASSERT_EQ(host.notes.size(), 1u);
    ASSERT(host.notes[0].find("retried without tools") != std::string::npos);
    ASSERT_TRUE(host.statuses.empty());
}

TEST(repair_template_parser_without_tools_reports_without_a_retry) {
    FakeRepairHost host;
    const auto repair = agent::make_template_parser_repair();
    ASSERT_TRUE(agent::RetryRequest::None ==
                repair->apply(kTemplateParserError, "generation", false, host));
    // The failure is still recorded; it just is not repairable this way.
    ASSERT_EQ(host.errors.size(), 1u);
    ASSERT_TRUE(host.notes.empty());
}

TEST(repair_server_model_switches_to_the_first_advertised_model) {
    FakeRepairHost host;
    agent::ModelInfo advertised;
    advertised.id = "server/known";
    advertised.context = 8192;
    host.models.push_back(advertised);
    const auto repair = agent::make_server_model_repair();
    ASSERT_TRUE(agent::RetryRequest::WithTools ==
                repair->apply(kModelNameError, "generation", true, host));
    ASSERT_EQ(host.adopted_model, "server/known");
    ASSERT_EQ(host.adopted_caps.context, 8192);
    ASSERT_EQ(host.statuses.size(), 1u);
    ASSERT(host.statuses[0].find("server/known") != std::string::npos);
    ASSERT_EQ(host.notes.size(), 1u);
    ASSERT(host.notes[0].find("server/known") != std::string::npos);
}

TEST(repair_server_model_declines_when_it_already_uses_that_model) {
    FakeRepairHost host;
    agent::ModelInfo same;
    same.id = host.configured_model;
    host.models.push_back(same);
    const auto repair = agent::make_server_model_repair();
    ASSERT_TRUE(agent::RetryRequest::None ==
                repair->apply(kModelNameError, "generation", true, host));
    ASSERT_EQ(host.errors.size(), 1u); // the rejection is still recorded
    ASSERT_TRUE(host.adopted_model.empty());
    ASSERT_TRUE(host.notes.empty());
}

TEST(repair_server_model_declines_with_no_model_list) {
    FakeRepairHost host;
    const auto repair = agent::make_server_model_repair();
    ASSERT_TRUE(agent::RetryRequest::None ==
                repair->apply(kModelNameError, "generation", true, host));
    ASSERT_EQ(host.errors.size(), 1u);
    ASSERT_TRUE(host.adopted_model.empty());
}

TEST(repair_api_key_swaps_the_key) {
    FakeRepairHost host;
    host.key = "sk-replacement";
    const auto repair = agent::make_api_key_repair();
    ASSERT_TRUE(agent::RetryRequest::WithTools ==
                repair->apply(kAuthError, "compression", true, host));
    ASSERT_EQ(host.adopted_key, "sk-replacement");
    ASSERT_EQ(host.stages.size(), 1u);
    ASSERT_EQ(host.stages[0], "compression");
    ASSERT_EQ(host.notes.size(), 1u);
    ASSERT(host.notes[0].find("API key") != std::string::npos);
}

TEST(repair_api_key_declines_when_the_host_has_none) {
    FakeRepairHost host;
    const auto repair = agent::make_api_key_repair();
    ASSERT_TRUE(agent::RetryRequest::None == repair->apply(kAuthError, "generation", true, host));
    ASSERT_EQ(host.errors.size(), 1u);
    ASSERT_TRUE(host.adopted_key.empty());
    ASSERT_TRUE(host.notes.empty());
}

TEST(repair_ladder_walks_past_a_strategy_that_cannot_act) {
    agent::RepairLadder ladder;
    ladder.add(
        std::make_unique<StubRepair>(agent::RequestFailure::ModelName, agent::RetryRequest::None));
    ladder.add(std::make_unique<StubRepair>(agent::RequestFailure::ModelName,
                                            agent::RetryRequest::WithTools));
    FakeRepairHost host;
    ASSERT_TRUE(agent::RetryRequest::WithTools ==
                ladder.repair(kModelNameError, "generation", true, host));
}

TEST(repair_ladder_ignores_an_unclassified_failure) {
    agent::RepairLadder ladder = agent::make_default_repair_ladder();
    FakeRepairHost host;
    ASSERT_TRUE(agent::RetryRequest::None ==
                ladder.repair("HTTP 500 upstream", "generation", true, host));
    ASSERT_TRUE(host.errors.empty());
}

TEST(repair_default_ladder_covers_the_three_failures) {
    const agent::RepairLadder ladder = agent::make_default_repair_ladder();
    ASSERT_EQ(ladder.size(), 3u);
}

// ---------------------------------------------------------------------------
// Learn UI: store listing / remove / promote APIs ([LU-01], [LU-03], [LU-04],
// [LU-05])
// ---------------------------------------------------------------------------

namespace {

agent::ExperienceConfig learn_ec(const std::string& path) {
    agent::ExperienceConfig ec;
    ec.store_path = path;
    return ec;
}

} // namespace

// [LU-01] all_memories/all_skills return every item, score-sorted desc.
TEST(learn_store_listing_order) {
    std::string path = "/tmp/amber_learn_list.json";
    auto store = agent::make_memory_store(learn_ec(path));
    store->set_current_turn(5);
    agent::Memory low;
    low.name = "low";
    low.content = "low content";
    low.evidence_count = 1;
    low.last_confirm_turn = 5;
    agent::Memory high;
    high.name = "high";
    high.content = "high content";
    high.evidence_count = 4;
    high.last_confirm_turn = 5;
    high.promoted = true;
    store->upsert(low);
    store->upsert(high);

    auto mems = store->all_memories();
    ASSERT_EQ(mems.size(), 2u);
    ASSERT_EQ(mems[0].name, "high");
    ASSERT_EQ(mems[1].name, "low");

    agent::Skill sk;
    sk.name = "deploy";
    sk.content = "deploy steps";
    sk.evidence_count = 3;
    sk.last_confirm_turn = 5;
    store->upsert(sk);
    auto sks = store->all_skills();
    ASSERT_EQ(sks.size(), 1u);
    ASSERT_EQ(sks[0].name, "deploy");
    ASSERT(sks[0].promoted == false);
    std::remove(path.c_str());
}

// [LU-03] remove erases the item and persists across save/load.
TEST(learn_store_remove_persists) {
    std::string path = "/tmp/amber_learn_remove.json";
    auto store = agent::make_memory_store(learn_ec(path));
    agent::Memory m;
    m.name = "keep";
    m.content = "keep content";
    m.evidence_count = 2;
    store->upsert(m);
    agent::Memory gone;
    gone.name = "gone";
    gone.content = "gone content";
    gone.evidence_count = 2;
    store->upsert(gone);

    std::string gone_id;
    for (const auto& mem : store->all_memories())
        if (mem.name == "gone")
            gone_id = mem.id;
    ASSERT_FALSE(gone_id.empty());

    ASSERT_TRUE(store->remove(gone_id));
    ASSERT_TRUE(store->save(path));
    ASSERT_EQ(store->all_memories().size(), 1u);

    auto reloaded = agent::make_memory_store(learn_ec(path));
    ASSERT_TRUE(reloaded->load(path));
    auto mems = reloaded->all_memories();
    ASSERT_EQ(mems.size(), 1u);
    ASSERT_EQ(mems[0].name, "keep");
    std::remove(path.c_str());
}

// [LU-04] remove with an unknown id returns false and changes nothing.
TEST(learn_store_remove_unknown) {
    std::string path = "/tmp/amber_learn_remove_unknown.json";
    auto store = agent::make_memory_store(learn_ec(path));
    agent::Memory m;
    m.name = "only";
    m.content = "only content";
    store->upsert(m);
    ASSERT_FALSE(store->remove("zzz"));
    ASSERT_EQ(store->all_memories().size(), 1u);
    ASSERT_TRUE(store->save(path));
    auto reloaded = agent::make_memory_store(learn_ec(path));
    ASSERT_TRUE(reloaded->load(path));
    ASSERT_EQ(reloaded->all_memories().size(), 1u);
    std::remove(path.c_str());
}

// [LU-05] set_promoted flips the flag and persists; unknown id returns false.
TEST(learn_store_set_promoted_persists) {
    std::string path = "/tmp/amber_learn_pin.json";
    auto store = agent::make_memory_store(learn_ec(path));
    agent::Skill sk;
    sk.name = "deploy";
    sk.content = "deploy steps";
    store->upsert(sk);
    std::string id = store->all_skills()[0].id;

    ASSERT_TRUE(store->set_promoted(id, true));
    ASSERT_TRUE(store->all_skills()[0].promoted);
    ASSERT_TRUE(store->save(path));

    auto reloaded = agent::make_memory_store(learn_ec(path));
    ASSERT_TRUE(reloaded->load(path));
    ASSERT_TRUE(reloaded->all_skills()[0].promoted);

    ASSERT_TRUE(reloaded->set_promoted(id, false));
    ASSERT_FALSE(reloaded->all_skills()[0].promoted);
    ASSERT_FALSE(reloaded->set_promoted("zzz", true));
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Learn UI: Agent wrappers ([LU-03], [LU-05], [LU-08])
// ---------------------------------------------------------------------------

// [LU-03] learn_forget removes the item and persists through the Agent.
TEST(learn_agent_forget_persists) {
    std::string path = "/tmp/amber_learn_agent.json";
    std::remove(path.c_str());
    agent::Config cfg;
    cfg.experience_store_path = path;
    auto store = agent::make_memory_store(agent::load_experience_config(cfg));
    agent::Memory m;
    m.name = "keep";
    m.content = "keep content";
    m.evidence_count = 2;
    store->upsert(m);
    agent::Memory gone;
    gone.name = "gone";
    gone.content = "gone content";
    gone.evidence_count = 2;
    store->upsert(gone);
    store->save(path);

    agent::ToolRegistry reg;
    agent::Agent agent(cfg, reg, {}, {}, {}, std::move(store));
    auto* s = agent.memory_store();
    ASSERT(s != nullptr);
    ASSERT_TRUE(s->load(path));
    std::string gone_id;
    for (const auto& mem : s->all_memories())
        if (mem.name == "gone")
            gone_id = mem.id;
    ASSERT_FALSE(gone_id.empty());

    ASSERT_EQ(agent.learn_forget(gone_id), "");
    auto reloaded = agent::make_memory_store(agent::load_experience_config(cfg));
    ASSERT_TRUE(reloaded->load(path));
    ASSERT_EQ(reloaded->all_memories().size(), 1u);
    ASSERT_EQ(reloaded->all_memories()[0].name, "keep");
    std::remove(path.c_str());
}

// [LU-04] learn_forget with an unknown id errors and leaves the file alone.
TEST(learn_agent_forget_unknown) {
    std::string path = "/tmp/amber_learn_agent_unknown.json";
    std::remove(path.c_str());
    agent::Config cfg;
    cfg.experience_store_path = path;
    auto store = agent::make_memory_store(agent::load_experience_config(cfg));
    agent::Memory m;
    m.name = "only";
    m.content = "only content";
    store->upsert(m);
    store->save(path);

    agent::ToolRegistry reg;
    agent::Agent agent(cfg, reg, {}, {}, {}, std::move(store));
    ASSERT_TRUE(agent.memory_store()->load(path));
    std::string err = agent.learn_forget("zzz");
    ASSERT_FALSE(err.empty());
    ASSERT(err.find("no learned item with id") != std::string::npos);
    auto reloaded = agent::make_memory_store(agent::load_experience_config(cfg));
    ASSERT_TRUE(reloaded->load(path));
    ASSERT_EQ(reloaded->all_memories().size(), 1u);
    std::remove(path.c_str());
}

// [LU-05] learn_pin flips the promoted flag and persists through the Agent.
TEST(learn_agent_pin_persists) {
    std::string path = "/tmp/amber_learn_agent_pin.json";
    std::remove(path.c_str());
    agent::Config cfg;
    cfg.experience_store_path = path;
    auto store = agent::make_memory_store(agent::load_experience_config(cfg));
    agent::Skill sk;
    sk.name = "deploy";
    sk.content = "deploy steps";
    store->upsert(sk);
    store->save(path);

    agent::ToolRegistry reg;
    agent::Agent agent(cfg, reg, {}, {}, {}, std::move(store));
    ASSERT_TRUE(agent.memory_store()->load(path));
    std::string id = agent.memory_store()->all_skills()[0].id;

    ASSERT_EQ(agent.learn_pin(id, true), "");
    auto reloaded = agent::make_memory_store(agent::load_experience_config(cfg));
    ASSERT_TRUE(reloaded->load(path));
    ASSERT_TRUE(reloaded->all_skills()[0].promoted);
    std::remove(path.c_str());
}

// [LU-08] With no store the wrappers report the store as disabled.
TEST(learn_agent_store_disabled) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent agent(cfg, reg);
    ASSERT(agent.memory_store() == nullptr);
    ASSERT_FALSE(agent.learn_forget("x").empty());
    ASSERT_FALSE(agent.learn_pin("x", true).empty());
}

// ---------------------------------------------------------------------------
// Search exclusions: hidden/vendored dirs are skipped by default, but an
// explicit path inside one of them is honored (the capability is never
// removed, only defaulted away). [I-2]/[I-3]
// ---------------------------------------------------------------------------

namespace {

std::string make_exclusion_tree() {
    std::string dir = "/tmp/amber_search_excl";
    run_cmd("rm -rf " + dir);
    run_cmd("mkdir -p " + dir + "/.git " + dir + "/.amber " + dir + "/third_party " + dir + "/src");
    std::ofstream(dir + "/src/a.cpp") << "int the_marker_symbol() { return 1; }\n";
    std::ofstream(dir + "/.git/x.cpp") << "int the_marker_symbol() { return 2; }\n";
    std::ofstream(dir + "/.amber/y.json") << "the_marker_symbol\n";
    std::ofstream(dir + "/third_party/z.cpp") << "int the_marker_symbol() { return 3; }\n";
    return dir;
}

} // namespace

TEST(search_default_excludes_hidden_and_vendored) {
    std::string dir = make_exclusion_tree();
    auto be = agent::make_grep_backend();
    auto hits = be->search("the_marker_symbol", dir, "*.cpp", 100);
    ASSERT_EQ(hits.size(), 1u);
    ASSERT(hits[0].path.find("/src/a.cpp") != std::string::npos);
    run_cmd("rm -rf " + dir);
}

TEST(search_excludes_are_removable) {
    std::string dir = make_exclusion_tree();
    auto be = agent::make_grep_backend();
    // Dropping "third_party" from the excludes makes vendored code searchable.
    auto hits = be->search("the_marker_symbol", dir, "*.cpp", 100, {".amber", ".git"});
    bool saw_vendored = false;
    for (const auto& h : hits)
        if (h.path.find("/third_party/z.cpp") != std::string::npos)
            saw_vendored = true;
    ASSERT(saw_vendored);
    run_cmd("rm -rf " + dir);
}

// Tool-level: an explicit path inside an excluded dir honors the intent.
TEST(search_tool_explicit_path_inside_excluded) {
    std::string dir = make_exclusion_tree();
    agent::Workspace::set_root(dir);
    auto tool = agent::make_search_tool(agent::builtin_search_backend_provider());

    auto default_run = tool->execute({{"pattern", "the_marker_symbol"}, {"glob", "*.cpp"}});
    ASSERT_TRUE(default_run.ok);
    ASSERT(default_run.output.find("z.cpp") == std::string::npos);

    auto vendored = tool->execute(
        {{"pattern", "the_marker_symbol"}, {"glob", "*.cpp"}, {"path", "third_party"}});
    ASSERT_TRUE(vendored.ok);
    ASSERT(vendored.output.find("z.cpp") != std::string::npos);
    run_cmd("rm -rf " + dir);
}

// Search must not silently honor out-of-workspace paths: it is read-only and
// ungated, so an escape would hand the model arbitrary file contents.
TEST(search_tool_rejects_out_of_workspace_path) {
    std::string dir = make_exclusion_tree();
    std::string outside = "/tmp/amber_outside_search";
    agent::Workspace::set_root(dir);
    run_cmd("rm -rf " + outside + " && mkdir -p " + outside);
    std::ofstream(outside + "/secret.txt") << "needle_secret_marker\n";

    auto tool = agent::make_search_tool(agent::builtin_search_backend_provider());
    auto r = tool->execute({{"pattern", "needle_secret_marker"}, {"path", outside}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("workspace") != std::string::npos);
    run_cmd("rm -rf " + dir + " " + outside);
    agent::Workspace::set_root("."); // leave a valid root for later tests
}

// ---------------------------------------------------------------------------
// Environment card ([I-9]): a compact session-fixed system-prompt section
// telling the agent its OS, user + privilege, the workspace root (where bash
// commands run), the date, resources, and which tools exist — so it can act
// in its environment without probing.
// ---------------------------------------------------------------------------

TEST(environment_card_renders_compact) {
    // The card embeds the resolved config/state/cache paths, and the state root carries
    // a slug of the project root -- so its length depends on where the checkout and the
    // XDG roots happen to live. Pin both, or the size assertion measures the machine
    // instead of the card (a deep worktree used to fail it, and only there).
    XdgGuard guard("environment_card");
    struct RootGuard {
        std::string saved = agent::Workspace::project_root();
        ~RootGuard() { agent::Workspace::set_root(saved); }
    } root_guard;
    agent::Workspace::set_root("/tmp/amber_env_card_ws");

    agent::EnvironmentInfo info;
    info.os = "Ubuntu 24.04 (Linux 6.8.0-45-generic x86_64)";
    info.user = "jack";
    info.root = false;
    info.sudo_passwordless = false;
    info.workspace = "/home/jack/project";
    info.date = "2026-08-07";
    info.timezone = "UTC+2";
    info.resources = "8 cores \u00b7 16 GB RAM";
    info.tools = {"git", "python3", "make", "g++"};

    std::string card = agent::render_environment_card(info);
    ASSERT_FALSE(card.empty());
    ASSERT(card.find("## Environment") == 0);
    ASSERT(card.find("OS: Ubuntu 24.04") != std::string::npos);
    ASSERT(card.find("User: jack (non-root)") != std::string::npos);
    ASSERT(card.find("Workspace: /home/jack/project") != std::string::npos);
    ASSERT(card.find("Date: 2026-08-07 (UTC+2)") != std::string::npos);
    ASSERT(card.find("8 cores") != std::string::npos);
    ASSERT(card.find("Tools available: g++, git, make, python3") != std::string::npos);
    // Compact: well under a couple of hundred tokens.
    // 601 bytes with the roots pinned above; the bound is a budget for the card, not a
    // 1-byte cliff. A block that pushes past it is a decision about what every request
    // carries, so it should be deliberate.
    ASSERT(card.size() < 700u);
}

TEST(environment_card_reports_privilege) {
    agent::EnvironmentInfo info;
    info.user = "root";
    info.root = true;
    std::string card = agent::render_environment_card(info);
    ASSERT(card.find("User: root") != std::string::npos);

    info.user = "jack";
    info.root = false;
    info.sudo_passwordless = true;
    card = agent::render_environment_card(info);
    ASSERT(card.find("User: jack (non-root, passwordless sudo)") != std::string::npos);
}

TEST(environment_card_sorts_tools) {
    agent::EnvironmentInfo info;
    info.os = "Linux";
    info.tools = {"git", "docker", "make", "g++"};
    std::string card = agent::render_environment_card(info);
    size_t docker = card.find("docker");
    size_t gpp = card.find("g++");
    size_t git = card.find("git");
    size_t make = card.find("make");
    ASSERT_TRUE(docker != std::string::npos);
    ASSERT_TRUE(gpp != std::string::npos);
    ASSERT_TRUE(git != std::string::npos);
    ASSERT_TRUE(make != std::string::npos);
    bool ordered = docker < gpp && gpp < git && git < make;
    ASSERT_TRUE(ordered);
}

TEST(environment_card_omits_unknown_fields) {
    // A machine nothing is known about contributes no machine fields. Amber's
    // own layout is always with the card, because it is not a probe: it is
    // where the agent's own files are.
    agent::EnvironmentInfo info;
    std::string card = agent::render_environment_card(info);
    ASSERT(card.find("OS:") == std::string::npos);
    ASSERT(card.find("User:") == std::string::npos);
    ASSERT(card.find("Amber files: config ") != std::string::npos);
    info.os = "Linux";
    card = agent::render_environment_card(info);
    ASSERT(card.find("User:") == std::string::npos);
    ASSERT(card.find("Workspace:") == std::string::npos);
    ASSERT(card.find("Tools") == std::string::npos);
}

// On the test machine the probe must find the basics.
TEST(environment_probe_collects_facts) {
    auto info = agent::probe_environment();
    ASSERT_FALSE(info.os.empty());
    // os_string() reports the actual utsname sysname ("Linux" / "Darwin").
#ifdef __APPLE__
    ASSERT(info.os.find("Darwin") != std::string::npos);
#else
    ASSERT(info.os.find("Linux") != std::string::npos);
#endif
    ASSERT_FALSE(info.user.empty());
    ASSERT_EQ(info.workspace, agent::Workspace::workspace_root());
    ASSERT_FALSE(info.date.empty());
    ASSERT_FALSE(info.timezone.empty());
    ASSERT_FALSE(info.resources.empty());
    ASSERT(!info.tools.empty()); // git or python3 present in CI
    ASSERT_FALSE(agent::render_environment_card(info).empty());
}

// ---------------------------------------------------------------------------
// WS-A: deployable data paths. Data files must resolve from a system data
// directory (XDG /usr/share) so the packaged app works from any CWD, while
// dev workflows (CWD / binary dir) keep precedence.
// ---------------------------------------------------------------------------

namespace {

struct DataTree {
    std::string base;
    std::string cwd;
    std::string bin;
    std::string xdg;
    std::string saved_cwd;
    std::string saved_xdg;
    bool xdg_was_set = false;

    ~DataTree() {
        run_cmd("rm -rf " + base);
        if (!saved_cwd.empty() && chdir(saved_cwd.c_str()) != 0)
            std::fprintf(stderr, "DataTree: could not restore cwd to %s\n", saved_cwd.c_str());
        if (xdg_was_set)
            setenv("XDG_DATA_HOME", saved_xdg.c_str(), 1);
        else
            unsetenv("XDG_DATA_HOME");
    }
};

DataTree make_data_tree() {
    DataTree t;
    t.base = "/tmp/amber_dp_tree";
    t.cwd = t.base + "/cwd";
    t.bin = t.base + "/bin";
    t.xdg = t.base + "/xdg";
    run_cmd("rm -rf " + t.base);
    run_cmd("mkdir -p " + t.cwd + " " + t.bin + " " + t.xdg + "/amber/prompts");
    char buf[4096];
    if (getcwd(buf, sizeof buf))
        t.saved_cwd = buf;
    t.saved_xdg = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
    t.xdg_was_set = std::getenv("XDG_DATA_HOME") != nullptr;
    setenv("XDG_DATA_HOME", t.xdg.c_str(), 1);
    if (chdir(t.cwd.c_str()) != 0) {
        std::fprintf(stderr, "DataTree: could not enter %s\n", t.cwd.c_str());
        std::exit(EXIT_FAILURE);
    }
    return t;
}

std::string dp_argv0(const std::string& bin_dir) {
    return bin_dir + "/amber";
}

} // namespace

TEST(data_path_resolves_from_xdg_data_home) {
    DataTree t = make_data_tree();
    std::ofstream(t.xdg + "/amber/prompts/system.md") << "xdg";
    // Not in CWD, not next to the binary — must come from XDG_DATA_HOME.
    std::string got = agent::resolve_data_path("prompts/system.md", dp_argv0(t.bin).c_str());
    ASSERT_EQ(got, t.xdg + "/amber/prompts/system.md");
}

TEST(data_path_priority_cwd_bin_xdg) {
    DataTree t = make_data_tree();
    run_cmd("mkdir -p " + t.cwd + "/prompts " + t.bin + "/prompts");
    std::ofstream(t.cwd + "/prompts/system.md") << "cwd";
    std::ofstream(t.bin + "/prompts/system.md") << "bin";
    std::ofstream(t.xdg + "/amber/prompts/system.md") << "xdg";
    std::string argv0s = dp_argv0(t.bin);
    const char* argv0 = argv0s.c_str();

    auto read_marker = [](const std::string& p) {
        std::ifstream f(p);
        std::string line;
        std::getline(f, line);
        return line;
    };

    // CWD wins while present.
    std::string got = agent::resolve_data_path("prompts/system.md", argv0);
    ASSERT_EQ(read_marker(got), "cwd");

    // Remove CWD copy: binary dir wins.
    std::remove((t.cwd + "/prompts/system.md").c_str());
    got = agent::resolve_data_path("prompts/system.md", argv0);
    ASSERT_EQ(read_marker(got), "bin");

    // Remove binary copy: XDG data dir wins.
    std::remove((t.bin + "/prompts/system.md").c_str());
    got = agent::resolve_data_path("prompts/system.md", argv0);
    ASSERT_EQ(read_marker(got), "xdg");
}

TEST(data_path_returns_empty_when_missing) {
    DataTree t = make_data_tree();
    std::string got =
        agent::resolve_data_path("prompts/definitely_not_here.md", dp_argv0(t.bin).c_str());
    ASSERT_EQ(got, "");
}

// Regression: a relocatable FHS install keeps data in <prefix>/share/amber,
// NOT next to the binary. When the binary is <prefix>/bin/amber and argv0 is
// that file path, data_file_candidates must find <prefix>/share/amber via the
// sibling rule (<bin>/../share/amber). This is the Homebrew layout; a bug
// where the caller passed exe_dir() (a directory) instead of the executable
// file path made dirname_of strip the wrong component and broke the search —
// the installed amber then showed only the feed-merged slash commands.
TEST(data_path_finds_fhs_sibling_share_dir) {
    DataTree t = make_data_tree();
    // <prefix>/bin/amber (argv0) + <prefix>/share/amber/completions.json
    std::string prefix = t.base + "/prefix";
    std::string bin_dir = prefix + "/bin";
    std::string share_dir = prefix + "/share/amber";
    run_cmd("mkdir -p " + bin_dir + " " + share_dir);
    std::ofstream(share_dir + "/completions.json") << "{}";

    // argv0 is the executable FILE path (what bootstrap passes; the fix makes
    // the TUI pass the resolved exe path the same way).
    std::string argv0s = bin_dir + "/amber";
    const char* argv0 = argv0s.c_str();

    std::string got = agent::resolve_data_path("completions.json", argv0);
    // The sibling rule yields <bin>/../share/amber/completions.json (the ".."
    // is left literal but the OS resolves it); assert the file is found and
    // points into the share dir.
    ASSERT_FALSE(got.empty());
    ASSERT(got.find("share/amber/completions.json") != std::string::npos);
}

// The coding library is a DIRECTORY under the same data root as the prompts.
// The file resolver would happily return a same-named regular file, so the
// directory lookup filters by kind -- and it must use the same candidate order
// (sibling share dir included) as every other data file.
TEST(data_path_resolves_a_directory_through_the_sibling_rule) {
    DataTree t = make_data_tree();
    const std::string prefix = t.base + "/prefix";
    const std::string bin_dir = prefix + "/bin";
    const std::string lib_dir = prefix + "/share/amber/prompts/domains/coding/library";
    run_cmd("mkdir -p " + bin_dir + " " + lib_dir);
    std::ofstream(lib_dir + "/tags.md") << "# tags\n";
    std::string argv0s = bin_dir + "/amber";
    const char* argv0 = argv0s.c_str();

    const std::string got = agent::resolve_data_dir("prompts/domains/coding/library", argv0);
    ASSERT_FALSE(got.empty());
    ASSERT(got.find("share/amber/prompts/domains/coding/library") != std::string::npos);

    ASSERT_EQ(agent::resolve_data_dir("prompts/domains/coding/absent", argv0), std::string());
}

// ---------------------------------------------------------------------------
// WS-A: fail-fast bootstrap. The app must not start when critical data files
// are missing; the validator reports each missing file and the locations
// searched.
// ---------------------------------------------------------------------------

TEST(bootstrap_validator_reports_missing_files) {
    DataTree t = make_data_tree();
    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";

    auto missing = agent::missing_bootstrap_files(cfg, dp_argv0(t.bin).c_str(), false);
    ASSERT_EQ(missing.size(), 2u);
    ASSERT(missing[0].find("system") != std::string::npos);
    ASSERT(missing[1].find("tools") != std::string::npos);
}

// The missing-file diagnostic must be self-explanatory: it lists every
// searched path with an existence annotation, the install prefix the app
// believes it lives in, and a hint — so a broken install is diagnosable from
// the message alone.
TEST(bootstrap_validator_reports_searched_paths_and_hint) {
    DataTree t = make_data_tree();
    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    std::string argv0 = dp_argv0(t.bin);
    auto missing = agent::missing_bootstrap_files(cfg, argv0.c_str(), false);
    ASSERT_EQ(missing.size(), 1u); // only the prompt (completions not required)
    const std::string& m = missing[0];
    ASSERT(m.find("system prompt") != std::string::npos);
    ASSERT(m.find("searched:") != std::string::npos);
    ASSERT(m.find(t.bin) != std::string::npos); // argv0-dir candidate listed
    ASSERT(m.find("[missing]") != std::string::npos);
    ASSERT(m.find("expected:") != std::string::npos); // install-prefix hint
    ASSERT(m.find("hint:") != std::string::npos);
}

TEST(bootstrap_validator_passes_when_files_exist) {
    DataTree t = make_data_tree();
    run_cmd("mkdir -p " + t.cwd + "/prompts " + t.xdg + "/amber");
    std::ofstream(t.cwd + "/prompts/system.md") << "x";
    std::ofstream(t.cwd + "/prompts/tools.md") << "x";
    std::ofstream(t.xdg + "/amber/completions.json") << "{}";

    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";

    // Prompts found in CWD; completions.json must come from XDG.
    auto missing = agent::missing_bootstrap_files(cfg, dp_argv0(t.bin).c_str(), true);
    ASSERT_EQ(missing.size(), 0u);
}

TEST(bootstrap_validator_requires_completions_when_requested) {
    DataTree t = make_data_tree();
    run_cmd("mkdir -p " + t.cwd + "/prompts");
    std::ofstream(t.cwd + "/prompts/system.md") << "x";
    std::ofstream(t.cwd + "/prompts/tools.md") << "x";

    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";

    // completions.json exists nowhere; TUI requires it.
    auto missing = agent::missing_bootstrap_files(cfg, dp_argv0(t.bin).c_str(), true);
    ASSERT_EQ(missing.size(), 1u);
    ASSERT(missing[0].find("completions") != std::string::npos);
}

TEST(parse_model_list_dedupes_ids) {
    std::string body = R"({"data": [
        {"id": "qwen35-moe"},
        {"id": "qwen35-moe"},
        {"id": "qwopus-27b"},
        {"id": "qwopus-27b"},
        {"id": "gemma4-12b-q4"}]})";
    std::vector<std::string> models;
    for (const auto& m : agent::make_dialect("openai")->parse_model_list_response(body))
        models.push_back(m.id);
    ASSERT_EQ(models.size(), 3u);
    bool saw_qwopus = false, saw_qwen = false;
    for (const auto& m : models) {
        if (m == "qwopus-27b")
            saw_qwopus = true;
        if (m == "qwen35-moe")
            saw_qwen = true;
    }
    ASSERT(saw_qwopus && saw_qwen);
}

// [I-7] Explicit 0 must disable the turn gates instead of silently keeping
// the pipeline defaults (min_turns=10, cooldown=20).
TEST(compression_config_explicit_zero_disables_gates) {
    agent::Config cfg;
    cfg.compression_min_turns_explicit = true;
    cfg.compression_min_turns = 0;
    cfg.compression_cooldown_turns_explicit = true;
    cfg.compression_cooldown_turns = 0;
    auto cc = agent::load_compression_config(cfg);
    ASSERT_EQ(cc.min_turns, 0);
    ASSERT_EQ(cc.cooldown_turns, 0);
}

TEST(compression_config_unset_keeps_defaults) {
    agent::Config cfg;
    auto cc = agent::load_compression_config(cfg);
    ASSERT_EQ(cc.min_turns, 10);
    ASSERT_EQ(cc.cooldown_turns, 20);
}

TEST(compression_gate_min_turns_zero_passes_immediately) {
    agent::CompressionConfig cc;
    cc.threshold = 0.01;
    cc.min_turns = 0;
    cc.cooldown_turns = 0;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 1000;
    agent::Context ctx;
    for (int i = 0; i < 3; ++i)
        ctx.push(msg("user", std::string(200, 'a')));
    ASSERT(gate->should_compress(ctx, cfg));
}

// ---------------------------------------------------------------------------
// [I-8] Skills archive installer: a tar.gz pack (local path or URL) with a
// SKILL.md at its root installs into a skill directory and becomes
// discoverable; archives without a valid SKILL.md are rejected.
// ---------------------------------------------------------------------------

namespace {

std::string make_skill_archive(const std::string& base) {
    run_cmd("mkdir -p " + base + "/src/skilltest");
    std::ofstream(base + "/src/skilltest/SKILL.md")
        << "---\nname: skilltest\ndescription: test skill\n---\n\n# skilltest\n\nDo the thing.\n";
    std::ofstream(base + "/src/skilltest/helper.txt") << "auxiliary data\n";
    std::string archive = base + "/pack.tar.gz";
    run_cmd("tar -czf " + archive + " -C " + base + "/src .");
    return archive;
}

} // namespace

TEST(skill_install_from_archive) {
    std::string base = "/tmp/amber_skill_install";
    run_cmd("rm -rf " + base);
    std::string archive = make_skill_archive(base);
    std::string dest = base + "/dest";

    std::string err = agent::install_skill_pack(archive, dest);
    ASSERT(err.empty());
    ASSERT(std::filesystem::exists(dest + "/skilltest/SKILL.md"));
    ASSERT(std::filesystem::exists(dest + "/skilltest/helper.txt"));

    auto files = agent::scan_skill_dir(dest, agent::SkillScope::Global);
    bool found = false;
    for (const auto& f : files)
        if (f.name == "skilltest")
            found = true;
    ASSERT(found);

    err = agent::uninstall_skill("skilltest", dest);
    ASSERT(err.empty());
    ASSERT(!std::filesystem::exists(dest + "/skilltest"));
    run_cmd("rm -rf " + base);
}

TEST(skill_install_rejects_non_skill_archive) {
    std::string base = "/tmp/amber_skill_install";
    run_cmd("rm -rf " + base);
    run_cmd("mkdir -p " + base + "/src/notaskill");
    std::ofstream(base + "/src/notaskill/README.md") << "no skill here\n";
    std::string archive = base + "/pack.tar.gz";
    run_cmd("tar -czf " + archive + " -C " + base + "/src .");

    std::string err = agent::install_skill_pack(archive, base + "/dest");
    ASSERT(!err.empty());
    ASSERT(err.find("SKILL.md") != std::string::npos);
    run_cmd("rm -rf " + base);
}

// ---------------------------------------------------------------------------
// System skills: contributed by code (a bundled plugin), not by a file
// ---------------------------------------------------------------------------

namespace {

// A catalog whose scan roots are all scratch directories, so a test can never
// read the developer's real skills.
std::unique_ptr<agent::SkillCatalog> scratch_catalog(const std::string& name) {
    agent::Workspace::set_root("/tmp/amber_sysskill_" + name + "/ws");
    agent::SkillScanPaths paths;
    paths.project = "/tmp/amber_sysskill_" + name + "/ws/skills";
    paths.global = "/tmp/amber_sysskill_" + name + "/cfg/skills";
    agent::Config cfg;
    return std::make_unique<agent::SkillCatalog>(cfg, paths,
                                                 "/tmp/amber_sysskill_" + name + "/cfg");
}

agent::SystemSkill make_system_skill(const std::string& name, const std::string& body) {
    agent::SystemSkill s;
    s.name = name;
    s.description = "test skill " + name;
    s.body = [body] { return body; };
    return s;
}

} // namespace

TEST(system_skill_is_discovered_readable_and_unwound) {
    run_cmd("rm -rf /tmp/amber_sysskill_discover");
    agent::register_system_skill(make_system_skill("sys-test", "# sys body\n"), "unit-test-owner");

    auto catalog = scratch_catalog("discover");
    catalog->discover({});
    auto entry = catalog->lookup("sys-test");
    ASSERT(entry.has_value());
    ASSERT(entry->scope == agent::SkillScope::System);

    // Indexed in the discovery slot (one line) and readable on demand.
    bool indexed = false;
    for (const auto& line : catalog->discovery_block())
        if (line.rfind("sys-test: ", 0) == 0)
            indexed = true;
    ASSERT(indexed);
    auto body = catalog->read_body("sys-test");
    ASSERT(body.has_value());
    ASSERT(body->find("# sys body") != std::string::npos);

    // Disabling the owning plugin takes the skill back out.
    agent::unregister_system_skills_for("unit-test-owner");
    catalog->refresh();
    ASSERT(!catalog->lookup("sys-test").has_value());
}

TEST(authored_skill_shadows_a_system_skill_of_the_same_name) {
    run_cmd("rm -rf /tmp/amber_sysskill_shadow");
    run_cmd("mkdir -p /tmp/amber_sysskill_shadow/ws/skills/sys-shadow");
    {
        std::ofstream f("/tmp/amber_sysskill_shadow/ws/skills/sys-shadow/SKILL.md");
        f << "---\nname: sys-shadow\ndescription: the project's own\n---\nproject body\n";
    }
    agent::register_system_skill(make_system_skill("sys-shadow", "system body\n"),
                                 "unit-test-owner");

    auto catalog = scratch_catalog("shadow");
    catalog->discover({});
    auto entry = catalog->lookup("sys-shadow");
    ASSERT(entry.has_value());
    // The project's copy wins: a user must be able to override what amber
    // ships with their own file.
    ASSERT(entry->scope == agent::SkillScope::Project);
    auto body = catalog->read_body("sys-shadow");
    ASSERT(body.has_value());
    ASSERT(body->find("project body") != std::string::npos);

    agent::unregister_system_skills_for("unit-test-owner");
}

// ---------------------------------------------------------------------------
// amber's own layout (config_paths) and the skill that documents it
// ---------------------------------------------------------------------------

TEST(config_paths_resolves_the_layout) {
    XdgGuard xdg("config_paths");
    run_cmd("rm -rf /tmp/amber_cfg_paths_ws");
    agent::Workspace::set_root("/tmp/amber_cfg_paths_ws");

    std::map<std::string, std::string> rows;
    for (const auto& p : agent::config_paths())
        rows[p.what] = p.path;

    // Global: the XDG config root. State: the XDG state root, keyed by the
    // project. Cache: the XDG cache root.
    ASSERT_EQ(rows["providers"], xdg.dir + "/amber/providers");
    ASSERT(rows["state"].find(xdg.state + "/amber/projects/") == 0);
    ASSERT_EQ(rows["cache"], xdg.cache + "/amber");
    // The project tree holds only what a repository may share.
    ASSERT_EQ(rows["project-config"], "/tmp/amber_cfg_paths_ws/.amber");
    ASSERT(rows["sessions"].find(rows["state"]) == 0);
    ASSERT(rows["settings"].find(rows["state"]) == 0);
    ASSERT(rows["experience"].find(rows["state"]) == 0);
    ASSERT(rows["policy"].find(rows["state"]) == 0);
    // The layout is a readout, not a side effect.
    ASSERT(!std::filesystem::exists("/tmp/amber_cfg_paths_ws"));
}

// The bundled amber-config skill is the agent-facing half of /get config
// paths; both render the same table, so the documented paths ARE the resolved
// paths -- a body that restated them in prose would drift on the first move.
TEST(amber_config_skill_documents_the_resolved_paths) {
    XdgGuard xdg("amber_config_skill");
    agent::Workspace::set_root("/tmp/amber_cfg_skill_ws");
    const std::string body = agent::plugins::render_amber_config_skill();
    for (const auto& p : agent::config_paths()) {
        if (body.find(p.path) == std::string::npos) {
            std::cerr << "missing path in skill body: " << p.what << " " << p.path << "\n";
        }
        ASSERT(body.find(p.path) != std::string::npos);
    }
    // It must also say what the reader can DO, not only where things are.
    ASSERT(body.find("/get config paths") != std::string::npos);
    ASSERT(body.find("/provider") != std::string::npos);
    ASSERT(body.find("/set model") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The error journal: what the app could not do, in one place
// ---------------------------------------------------------------------------

// Diagnostics are opt-in: with the switch off there is no journal at all, while
// the session transcript is written either way.
TEST(error_journal_is_absent_until_diagnostics_are_on) {
    const std::string dir = "/tmp/amber_error_journal_off";
    run_cmd("rm -rf " + dir);
    std::filesystem::create_directories(dir);

    agent::ConversationLog log;
    log.open(dir + "/transcript.jsonl");
    log.event("error", {{"reason", "internal_failure"}, {"error", "boom"}});

    ASSERT(log.enabled());
    ASSERT_FALSE(log.errors_enabled());
    ASSERT(std::filesystem::exists(dir + "/transcript.jsonl"));
    ASSERT(!std::filesystem::exists(dir + "/errors.jsonl"));
    run_cmd("rm -rf " + dir);
}

// An app failure lands in the journal; a failed tool call is the model's
// business and stays in the transcript only.
TEST(error_journal_keeps_app_failures_and_only_those) {
    const std::string dir = "/tmp/amber_error_journal_on";
    run_cmd("rm -rf " + dir);
    std::filesystem::create_directories(dir);

    agent::ConversationLog log;
    log.open(dir + "/transcript.jsonl");
    log.open_errors(dir + "/errors.jsonl");
    log.event("chat_error", {{"stage", "generation"}, {"error", "type_error.302"}});
    log.event("error", {{"reason", "uncaught"}, {"error", "host callback exploded"}});
    log.event("tool_result", {{"name", "bash"}, {"ok", false}});
    log.event("user", {{"content", "hi"}});

    ASSERT(log.errors_enabled());
    const auto read_all = [](const std::string& path) {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string journal = read_all(dir + "/errors.jsonl");
    ASSERT(journal.find("type_error.302") != std::string::npos);
    ASSERT(journal.find("host callback exploded") != std::string::npos);
    ASSERT(journal.find("tool_result") == std::string::npos);
    ASSERT(journal.find("\"user\"") == std::string::npos);
    // The same failures are still in the session transcript.
    const std::string transcript = read_all(dir + "/transcript.jsonl");
    ASSERT(transcript.find("type_error.302") != std::string::npos);
    ASSERT(transcript.find("tool_result") != std::string::npos);
    run_cmd("rm -rf " + dir);
}

// The journal is a window on what just went wrong, not an archive: past the
// budget it rotates, keeping one previous file.
TEST(error_journal_rotates_instead_of_growing) {
    const std::string dir = "/tmp/amber_error_journal_rotate";
    run_cmd("rm -rf " + dir);
    std::filesystem::create_directories(dir);

    agent::ConversationLog log;
    log.open(dir + "/transcript.jsonl");
    log.open_errors(dir + "/errors.jsonl");
    const std::string payload(512, 'x');
    for (int i = 0; i < 2300; ++i)
        log.event("error", {{"reason", "wall_clock_exceeded"}, {"error", payload}});

    ASSERT(std::filesystem::exists(dir + "/errors.jsonl"));
    ASSERT(std::filesystem::exists(dir + "/errors.jsonl.1"));
    // Neither file is left unbounded: the live one is under the budget.
    ASSERT(std::filesystem::file_size(dir + "/errors.jsonl") < 1024 * 1024);
    run_cmd("rm -rf " + dir);
}

TEST(skill_install_rejects_malformed_skill) {
    std::string base = "/tmp/amber_skill_install";
    run_cmd("rm -rf " + base);
    run_cmd("mkdir -p " + base + "/src/skilltest");
    std::ofstream(base + "/src/skilltest/SKILL.md") << "no frontmatter at all\n";
    std::string archive = base + "/pack.tar.gz";
    run_cmd("tar -czf " + archive + " -C " + base + "/src .");

    std::string err = agent::install_skill_pack(archive, base + "/dest");
    ASSERT(!err.empty());
    run_cmd("rm -rf " + base);
}

// [I-5 residual ③] Live MCP tools reflect into the completion tree.
TEST(mcp_completion_subtree_reflects_live_tools) {
    agent::ToolRegistry reg;
    // Minimal fake tools named like registered MCP adapters.
    struct FakeMcpTool : agent::Tool {
        std::string id;
        std::string name() const noexcept override { return id; }
        std::string description() const noexcept override { return "desc of " + id; }
        agent::json parameters_schema() const override { return agent::json::object(); }
        agent::ToolResult execute(const agent::json&) const override { return {}; }
    };
    auto add = [&](const std::string& n) {
        auto t = std::make_unique<FakeMcpTool>();
        t->id = n;
        reg.register_tool(std::move(t));
    };
    add("mcp_github_list_issues");
    add("mcp_github_get_issue");
    add("read"); // non-mcp tool must be ignored

    auto subtree = agent::mcp_completion_subtree(reg);
    ASSERT(subtree.contains("mcp"));
    const auto& srv = subtree["mcp"]["children"]["github"];
    ASSERT(srv.contains("children"));
    ASSERT(srv["children"].contains("list_issues"));
    ASSERT(srv["children"]["list_issues"]["help"] == "desc of mcp_github_list_issues");
    ASSERT_EQ(srv["children"].size(), 2u);
}

// [GR] Graceful recovery for request-side 400s: classify what the server
// rejected so the turn can adapt instead of dying.
TEST(request_failure_classifier) {
    using agent::RequestFailure;
    ASSERT(agent::classify_request_failure(
               "HTTP 400 from LLM server: {\"error\":{\"code\":400,\"message\":"
               "\"Unable to generate parser for this template. Automatic parser "
               "generation failed\"}}") == RequestFailure::TemplateParser);
    ASSERT(agent::classify_request_failure(
               "HTTP 400 from LLM server: {\"error\":\"The model ornith-35b does "
               "not exist\"}") == RequestFailure::ModelName);
    ASSERT(agent::classify_request_failure("HTTP 401 from LLM server: bad key") ==
           RequestFailure::Auth);
    ASSERT(agent::classify_request_failure("HTTP 403 from LLM server: forbidden") ==
           RequestFailure::Auth);
    ASSERT(agent::classify_request_failure("HTTP 400 from LLM server: context overflow") ==
           RequestFailure::None);
}

// [GR] Tool schemas are sanitized before they reach the server so its grammar
// builder never sees null types or arrays without items (llama.cpp 400s with
// "type must be array, but is null").
TEST(tool_schema_sanitizer) {
    using agent::json;
    json s = {{"type", "object"},
              {"properties",
               {{"tags", {{"type", "array"}, {"items", nullptr}}},
                {"count", {{"type", "array"}}},
                {"nested", {{"type", "object"}, {"properties", {{"x", {{"type", nullptr}}}}}}},
                {"bare", nullptr}}}};
    agent::sanitize_tool_schema(s);
    ASSERT(s["properties"]["tags"]["items"].is_object());
    ASSERT(s["properties"]["count"]["items"].is_object());
    ASSERT(s["properties"]["nested"]["properties"]["x"]["type"] == "object");
    ASSERT(s["properties"]["bare"]["type"] == "object");
}

// The nlohmann {"required", {}} gotcha emits "required": null, which breaks
// llama.cpp's grammar generator ("type must be array, but is null" 400).
// Sanitizing must repair it before the request leaves the client.
TEST(tool_schema_sanitizer_repairs_null_required) {
    agent::json s = {{"type", "object"},
                     {"properties", {{"name", {{"type", "string"}}}}},
                     {"required", nullptr}};
    agent::sanitize_tool_schema(s);
    ASSERT(s["required"].is_array());
    ASSERT_EQ(s["required"].size(), 0u);
}

// [TC] Attribute-style XML tool calls (ornith template):
//   <tool_call>
//   <function=bash>
//   <parameter=command>
//   find . -type f
//   </parameter>
//   </function>
//   </tool_call>
// ---------------------------------------------------------------------------
// Patterns 1 (<tool_call><name>/<arguments>) and 2 (<function><name>) had no
// test coverage at all before the parser was split into one function per
// dialect. Every expectation below was recorded from the implementation
// *before* the split, so this table is the equivalence check for it.
// ---------------------------------------------------------------------------

TEST(tool_call_parser_xml_name_arguments) {
    std::string text = "<tool_call><name>bash</name>"
                       "<arguments>{\"command\":\"ls\"}</arguments></tool_call>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "bash");
    // The arguments stay the raw JSON *string*: the caller parses them.
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"command\":\"ls\"}");
}

TEST(tool_call_parser_xml_name_arguments_multiple) {
    std::string text = "<tool_call><name>bash</name>"
                       "<arguments>{\"command\":\"ls\"}</arguments></tool_call>"
                       "<tool_call><name>read</name>"
                       "<arguments>{\"path\":\"a\"}</arguments></tool_call>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "bash");
    ASSERT_EQ(calls[1]["function"]["name"].get<std::string>(), "read");
}

TEST(tool_call_parser_xml_name_without_arguments_is_not_a_call) {
    // A <name> with no <arguments> falls through to the JSON sub-pattern, which
    // cannot parse XML — so nothing is extracted.
    ASSERT_TRUE(
        agent::extract_tool_calls_from_text("<tool_call><name>bash</name></tool_call>").is_null());
}

TEST(tool_call_parser_json_inside_tool_call) {
    // The whole <tool_call> block may be JSON instead of <name>/<arguments>.
    std::string text =
        "<tool_call>{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}</tool_call>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "bash");
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"command\":\"ls\"}");
}

TEST(tool_call_parser_function_block) {
    // Pattern 2: <function><name>X</name>BODY</function>, with BODY as JSON.
    auto calls = agent::extract_tool_calls_from_text(
        "<function><name>bash</name>{\"command\":\"ls\"}</function>");
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "bash");
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"command\":\"ls\"}");
}

TEST(tool_call_parser_function_block_json_wrapper) {
    // A body of {"json": {...}} unwraps to the inner object.
    auto calls = agent::extract_tool_calls_from_text(
        "<function><name>bash</name>{\"json\":{\"command\":\"ls\"}}</function>");
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"command\":\"ls\"}");
}

TEST(tool_call_parser_function_block_multiple_and_unclosed) {
    auto calls = agent::extract_tool_calls_from_text(
        "<function><name>bash</name>{\"command\":\"a\"}</function>"
        "<function><name>read</name>{\"path\":\"b\"}</function>");
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[1]["function"]["name"].get<std::string>(), "read");

    // A call the model cut off mid-stream still parses; the body is whatever
    // remains, which here is empty.
    auto unclosed = agent::extract_tool_calls_from_text("<function><name>bash</name>");
    ASSERT_TRUE(!unclosed.is_null());
    ASSERT_EQ(unclosed.size(), 1u);
    ASSERT_EQ(unclosed[0]["function"]["arguments"].get<std::string>(), "");
}

TEST(tool_call_parser_no_dialect_found_returns_null) {
    ASSERT_TRUE(agent::extract_tool_calls_from_text("").is_null());
    ASSERT_TRUE(agent::extract_tool_calls_from_text("just prose, no calls").is_null());
}

TEST(tool_call_parser_attribute_style) {
    std::string text = "I'll review the app.\n"
                       "<tool_call>\n"
                       "<function=bash>\n"
                       "<parameter=command>\n"
                       "find . -type f | head\n"
                       "</parameter>\n"
                       "</function>\n"
                       "</tool_call>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "bash");
    ASSERT_EQ(calls[0]["function"]["arguments"]["command"].get<std::string>(),
              "find . -type f | head");
}

TEST(tool_call_parser_attribute_style_multiple) {
    std::string text = "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n"
                       "</parameter>\n</function>\n</tool_call>\n"
                       "<tool_call>\n<function=search>\n<parameter=pattern>\nTODO\n"
                       "</parameter>\n</function>\n</tool_call>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "read");
    ASSERT_EQ(calls[1]["function"]["name"].get<std::string>(), "search");
    ASSERT_EQ(calls[1]["function"]["arguments"]["pattern"].get<std::string>(), "TODO");
}

TEST(tool_call_parser_attribute_style_unclosed) {
    // Model cut off mid-call: no closing </tool_call>.
    std::string text = "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "read");
    ASSERT_EQ(calls[0]["function"]["arguments"]["path"].get<std::string>(), "Makefile");
}

// [TC] Hermes-style bare-JSON tool calls (Qwen2.5-Coder via llama.cpp):
// the model emits {"name":"read","arguments":{...}} as plain content text,
// without any XML wrapper. A call in the middle of prose must be extracted.
TEST(tool_call_parser_bare_json) {
    std::string text = "Let's read the task file.\n"
                       "{\"name\": \"read\", \"arguments\": {\"path\": \"TASK.md\"}}\n"
                       "I'll then write the solution.";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "read");
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"path\":\"TASK.md\"}");
}

// [TC] Multiple Hermes-style calls in one response, each with arguments,
// must all be extracted in order.
TEST(tool_call_parser_bare_json_multiple) {
    std::string text = "{\"name\": \"read\", \"arguments\": {\"path\": \"TASK.md\"}}\n"
                       "{\"name\": \"write\", \"arguments\": {\"path\": \"fizzbuzz.cpp\", "
                       "\"edits\": [{\"old\": \"\", \"new\": \"int main(){}\"}]}}";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "read");
    ASSERT_EQ(calls[1]["function"]["name"].get<std::string>(), "write");
}

// [TC] The 32B, when forced to use a tool, wraps the call in <tools> tags:
// <tools>{"name": "read", "arguments": {"path": "/etc/hostname"}}</tools>
TEST(tool_call_parser_bare_json_tools_wrapper) {
    std::string text = "<tools>{\"name\": \"read\", \"arguments\": {\"path\": \"/etc/hostname\"}}"
                       "</tools>";
    auto calls = agent::extract_tool_calls_from_text(text);
    ASSERT_TRUE(!calls.is_null());
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0]["function"]["name"].get<std::string>(), "read");
    ASSERT_EQ(calls[0]["function"]["arguments"].get<std::string>(), "{\"path\":\"/etc/hostname\"}");
}

// Binary files must be refused, not dumped as garbage lines (a NUL-byte
// binary read once produced 32k lines in the conversation).
TEST(read_tool_refuses_binary) {
    agent::Workspace::set_root(".");
    std::string dir = "read_bin_" + std::to_string(getpid());
    std::filesystem::create_directories(dir);
    std::ofstream f(dir + "/bin.dat", std::ios::binary);
    // strlen() stops at the first NUL - build the fixture with an explicit
    // length so the NUL bytes actually land in the file.
    f.write(std::string("\x00\x01\x02hello\n\x00world\n", 16).data(), 16);
    f.close();
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", dir + "/bin.dat"}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("binary") != std::string::npos);
    std::filesystem::remove_all(dir);
}

// The read limit is line-based and hard-capped so a request can never pull
// tens of thousands of lines into the conversation.
TEST(read_tool_clamps_limit) {
    agent::Workspace::set_root(".");
    std::string dir = "read_big_" + std::to_string(getpid());
    std::filesystem::create_directories(dir);
    std::ofstream f(dir + "/big.txt");
    for (int i = 0; i < 5000; ++i)
        f << "line " << i << "\n";
    f.close();
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", dir + "/big.txt"}, {"limit", 100000}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.meta["lines"].get<long>() <= 2000);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Todowrite tool (P1 — externalized task tracking)
// ---------------------------------------------------------------------------

TEST(todo_store_full_list_replace) {
    agent::TodoStore store;
    store.update({
        {"p1", "fix parsing", agent::TodoStatus::InProgress},
        {"p2", "write tests", agent::TodoStatus::Pending},
    });
    ASSERT_EQ(store.items().size(), 2u);
    ASSERT_EQ(store.items()[0].id, "p1");
    ASSERT_EQ(store.items()[1].text, "write tests");
    // Full-list replacement: a new update drops the previous list entirely.
    store.update({{"p3", "ship", agent::TodoStatus::Completed}});
    ASSERT_EQ(store.items().size(), 1u);
    ASSERT_EQ(store.items()[0].id, "p3");
    // Empty update clears.
    store.update({});
    ASSERT(store.items().empty());
}

TEST(todowrite_tool_replaces_and_echoes) {
    agent::TodoStore store;
    auto tool = agent::make_todowrite_tool(store);
    ASSERT_EQ(tool->name(), "todowrite");
    auto r = tool->execute({{"todos",
                             {
                                 {{"id", "p1"}, {"text", "fix parsing"}, {"status", "in_progress"}},
                                 {{"id", "p2"}, {"text", "write tests"}, {"status", "pending"}},
                             }}});
    ASSERT(r.ok);
    ASSERT(r.output.find("p1") != std::string::npos);
    ASSERT(r.output.find("fix parsing") != std::string::npos);
    ASSERT_EQ(store.items().size(), 2u);
    // Second call replaces the list entirely.
    tool->execute({{"todos", {{{"id", "p9"}, {"text", "ship it"}, {"status", "completed"}}}}});
    ASSERT_EQ(store.items().size(), 1u);
    ASSERT_EQ(store.items()[0].id, "p9");
}

TEST(todowrite_tool_rejects_invalid_status) {
    agent::TodoStore store;
    auto tool = agent::make_todowrite_tool(store);
    auto r =
        tool->execute({{"todos",
                        {
                            {{"id", "p1"}, {"text", "x"}, {"status", "done"}}, // not a valid status
                        }}});
    ASSERT_FALSE(r.ok);
    ASSERT(store.items().empty());
    // Missing required key also fails.
    auto r2 = tool->execute(agent::json::object());
    ASSERT_FALSE(r2.ok);
}

// The plan and task tools ship enabled like every other bundled tool plugin;
// what used to be a config flag is now plugin state, switched with
// `/set plugin tool_plan off`.
TEST(todowrite_and_task_ship_with_the_default_tool_set) {
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{});
    ASSERT(reg.find("todowrite") != nullptr);
    ASSERT(reg.find("task") != nullptr);
}

TEST(compression_gate_large_window_fires_at_threshold_fraction) {
    // The budget is the REAL window (probed or explicit): a 262k window with
    // threshold 0.5 fires only past ~131k tokens — the old unconditional 32k
    // cap was never part of the spec and is gone.
    agent::CompressionConfig cc;
    cc.threshold = 0.5;
    cc.cooldown_turns = 0;
    cc.min_turns = 2;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 262144;
    cfg.turn_counter = 5;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 20000; // ~7.6% of the window: below threshold
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 150000; // ~57% of the window: above threshold
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_explicit_window_honored) {
    // An explicitly configured window is the budget; the threshold is a
    // fraction of that window, never of a hidden cap.
    agent::CompressionConfig cc;
    cc.threshold = 0.7;
    cc.cooldown_turns = 0;
    cc.min_turns = 2;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 100000;
    cfg.turn_counter = 5;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 50000; // 50% of the window: below threshold
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 70000; // 70% of the window: at threshold
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_unknown_window_never_auto_fires) {
    // No probe, no explicit config: the gate must not guess a window — an
    // arbitrary fallback budget fired at a tiny fraction of modern 128k+
    // models ("compression at 8% of context"). Compression still runs via
    // /compress and the 400-overflow learner teaches the real window.
    agent::CompressionConfig cc;
    cc.threshold = 0.5;
    cc.cooldown_turns = 0;
    cc.min_turns = 2;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg; // context_size stays 0 (unknown)
    cfg.turn_counter = 5;
    cfg.user_turn_counter = cfg.turn_counter;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 15000;
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 100000; // even a huge estimate must not fire
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_ignores_the_turn_floor_near_saturation) {
    // A short history that is nearly full has nowhere left to grow: the message
    // floor (10) must not block it at 95% utilisation.
    agent::CompressionConfig cc;
    cc.threshold = 0.7;
    cc.min_turns = 10;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 1000;
    cfg.prompt_tokens_used = 950; // 95%: saturated
    cfg.user_turn_counter = 2;    // past the first-turn cooldown special case
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", std::string(3700, 'u')));
    ctx.push(msg("assistant", "working")); // 3 messages: under the floor, reducible
    ASSERT_EQ(ctx.size(), size_t{3});
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_first_compress_not_cooldown_blocked) {
    // last_compress_turn_ starts at 0 ("never compressed"); the cooldown must
    // not block the FIRST compression of a fresh session — only subsequent
    // ones. Previously (0 - 0) < cooldown blocked everything until turn 20.
    agent::CompressionConfig cc;
    cc.threshold = 0.5;
    cc.cooldown_turns = 20;
    cc.min_turns = 2;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 262144;
    cfg.turn_counter = 5;
    cfg.user_turn_counter = cfg.turn_counter;
    cfg.prompt_tokens_used = 200000; // > 50% of the window: threshold met
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    ASSERT(gate->should_compress(ctx, cfg));
    // After the first compression, cooldown applies normally.
    gate->set_last_compress_turn(5);
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.turn_counter = 26;
    cfg.user_turn_counter = cfg.turn_counter; // 5 + 20 + 1
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_rearms_on_growth_inside_one_turn) {
    // The cooldown is not a wall: a long turn that refills the context must be
    // able to compact again without waiting for a user turn that may never come.
    agent::CompressionConfig cc;
    cc.threshold = 0.7;
    cc.cooldown_turns = 20;
    cc.min_turns = 2;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 262144;
    cfg.user_turn_counter = 5;
    agent::Context ctx;
    ctx.push(msg("system", "s"));
    ctx.push(msg("user", "u"));
    ctx.push(msg("assistant", "working"));
    cfg.prompt_tokens_used = 200000;
    ASSERT(gate->should_compress(ctx, cfg));
    gate->set_last_compress_turn(5);
    cfg.user_turn_counter = 6;
    cfg.prompt_tokens_used = 200000; // inside cooldown, no new consumption
    ASSERT_FALSE(gate->should_compress(ctx, cfg));
    cfg.prompt_tokens_used = 230000; // a tenth of the window arrived
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(compression_gate_hermetic_conditions) {
    // The exact gate inputs of the k-01 hermetic scenario (runner's
    // context_size=4096, 10 context messages, turn 4, 3000 tokens used).
    agent::CompressionConfig cc;
    cc.cooldown_turns = 20;
    cc.min_turns = 10;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 4096;
    cfg.turn_counter = 4;
    cfg.user_turn_counter = cfg.turn_counter;
    cfg.prompt_tokens_used = 3000;
    agent::Context ctx;
    ctx.push(msg("system", "sys"));
    ctx.push(msg("user", "what is in a.txt"));
    for (int i = 0; i < 4; ++i) {
        ctx.push(msg("assistant", "x"));
        ctx.push(msg("tool", "content"));
    }
    ASSERT(gate->should_compress(ctx, cfg));
}

TEST(llm_reasoning_effort_in_request_body) {
    std::string body;
    int srv = spawn_mock_sse(8915, body);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8915/v1";
    cfg.stream = true;
    cfg.model = "o3-mini"; // documented OpenAI reasoning family, no listing metadata
    cfg.reasoning_effort = "high";
    agent::HttpLLMClient client(cfg);
    agent::Message reply = client.chat_stream({}, {}, [](const agent::StreamChunk&) {});
    ASSERT(body.find("\"reasoning_effort\":\"high\"") != std::string::npos);
    close(srv);
}

TEST(agent_set_reasoning_effort_rebuilds_client) {
    agent::Workspace::set_root("/tmp/amber_rsn_test");
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 10;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos);

    std::string factory_seen;
    agent::LLMClientFactory factory = [&factory_seen](const agent::Config& c) {
        factory_seen = c.reasoning_effort;
        return std::make_unique<agent::HttpLLMClient>(c);
    };
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, nullptr, factory);
    ag.set_reasoning_effort("high");
    ASSERT_EQ(factory_seen, "high");
}

// T1 — docs/fix-proposal/thread-safety-and-startup-hardening-2026-09-21.md.
//
// The UI thread reads the agent's config every frame (status bar, feeds, session
// snapshots) while the agent's worker mutates it: cold-start model resolution in
// Agent::run(), learned context size, per-turn stats. There is no lock and no
// snapshot, so a reader can observe a half-written std::string.
//
TEST(agent_config_read_is_race_free) {
    // A reader must never observe a value that was never published. The writer
    // only ever publishes "model-a" or "model-b" (the initial default is
    // "gpt-4o-mini"), so anything else is a read of a half-written std::string.
    agent::Workspace::set_root("/tmp/amber_cfg_race_test");
    agent::Config cfg;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos);
    agent::Agent ag(cfg, reg);

    std::atomic<bool> stop{false};
    std::atomic<int> unpublished{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto live = ag.config_snapshot(); // the render-path read
            if (!live)
                continue;
            const std::string& m = live->model;
            if (m != "gpt-4o-mini" && m != "model-a" && m != "model-b")
                unpublished.fetch_add(1);
        }
    });
    for (int i = 0; i < 200; ++i)
        ag.set_model(i % 2 ? "model-a" : "model-b");
    stop.store(true);
    reader.join();
    ASSERT_EQ(unpublished.load(), 0);
}

TEST(provider_custom_unconfigured_is_error) {
    // No user connection anywhere (no env, no amber.conf endpoint, no
    // custom global config): select("custom") must FAIL loudly — never
    // resolve to the Config DTO defaults (localhost:8000).
    setenv("XDG_CONFIG_HOME", "/tmp/amber_xdg_none", 1);
    unsetenv("AMBER_API_BASE");
    unsetenv("AMBER_API_KEY");
    unsetenv("AMBER_MODEL");
    agent::Config cfg;
    agent::ToolRegistry tools;
    agent::Workspace ws;
    agent::PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();
    auto svc = agent::make_default_provider_service(cfg);
    auto sel = svc->select("custom");
    unsetenv("XDG_CONFIG_HOME");
    ASSERT_FALSE(sel.ok());
    // Loud, actionable failure — never a silent fallback to the DTO
    // default endpoint, and never "unknown": the plugin contributes the
    // preset, so custom is a known provider.
    ASSERT(sel.error.find("no endpoint") != std::string::npos);
}

TEST(config_save_settings_persists_compression) {
    // /set compression threshold 0.8 must survive a restart: save_settings
    // writes the compression fields, load() restores them.
    std::string path = "/tmp/amber_compression_settings_test.conf";
    agent::Config c;
    c.compression_threshold = 0.8;
    c.compression_threshold_explicit = true;
    c.compression_min_turns = 4;
    c.compression_min_turns_explicit = true;
    c.compression_cooldown_turns = 12;
    c.compression_cooldown_turns_explicit = true;
    ASSERT(c.save_settings(path));

    agent::Config d;
    d.load(path);
    ASSERT(d.compression_threshold_explicit);
    ASSERT_EQ(d.compression_threshold, 0.8);
    ASSERT(d.compression_min_turns_explicit);
    ASSERT_EQ(d.compression_min_turns, 4);
    ASSERT(d.compression_cooldown_turns_explicit);
    ASSERT_EQ(d.compression_cooldown_turns, 12);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Thread-safety: registry + job lifecycle (top-5 item #2 races)
// ---------------------------------------------------------------------------

namespace {
// Minimal no-op tool for concurrency probes.
class ProbeTool : public agent::Tool {
public:
    explicit ProbeTool(std::string name) : name_(std::move(name)) {}
    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return "probe"; }
    agent::json parameters_schema() const override { return agent::json::object(); }
    agent::ToolResult execute(const agent::json&) const override { return {true, "ok", ""}; }

private:
    std::string name_;
};
} // namespace

// Concurrent register/find/snapshot_tools from dispatch workers and the host
// thread. Unsynchronized vector mutation is UB; the count assertion is the
// regression guard (TSan is the authoritative detector, not run in CI).
TEST(registry_concurrent_register_find) {
    agent::ToolRegistry reg;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&reg, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                reg.register_tool(std::make_unique<ProbeTool>("probe_" + std::to_string(t) + "_" +
                                                              std::to_string(i)));
                (void)reg.find("probe_0_0");
                (void)reg.snapshot_tools();
            }
        });
    for (auto& th : threads)
        th.join();
    ASSERT_EQ(reg.snapshot_tools().size(), static_cast<size_t>(kThreads * kPerThread));
    ASSERT(reg.find("probe_7_24") != nullptr);
}

// JobService::get returns a shared reference: a caller holding it must stay
// safe when the service erases the job (concurrent stop()/kill).
TEST(job_service_get_holds_live_reference) {
    agent::JobService jobs;
    std::string id = jobs.start("echo hello", "", 60, 30);
    ASSERT(!id.empty());
    std::shared_ptr<agent::Job> j = jobs.get(id);
    ASSERT(j != nullptr);
    ASSERT(jobs.stop(id));
    // The held job was killed but its snapshot stays queryable — no dangling
    // pointer into the service's map.
    agent::JobInfo info = j->info();
    ASSERT(info.state != agent::JobState::Failed);
}

// A held tool lease must survive a concurrent unregister: dispatch keeps the
// shared_ptr through execute() while the host removes the tool.
TEST(registry_lease_survives_unregister) {
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<ProbeTool>("probe_lease"));
    std::shared_ptr<agent::Tool> held = reg.find("probe_lease");
    ASSERT(held != nullptr);
    ASSERT_EQ(reg.unregister_tools_with_prefix("probe_"), 1u);
    ASSERT(reg.find("probe_lease") == nullptr);
    auto r = held->execute(agent::json::object());
    ASSERT(r.ok);
}

// A child that closes stdout but keeps running must be terminated and reaped
// by the EOF path — never marked Done with a live orphan (which would zombie).
TEST(job_eof_daemon_is_terminated) {
    agent::Workspace::set_root("/tmp/amber_job_ws");
    agent::JobService jobs;
    std::string id = jobs.start("exec 1>&- 2>&-; sleep 30", "/tmp/amber_job_ws", 60, 30);
    ASSERT(!id.empty());
    // Poll past the 2s reap grace for the EOF path to force-kill the group —
    // a fixed sleep is racy on slow CI runners (macOS process teardown can
    // exceed 3.5s).
    auto j = jobs.get(id);
    ASSERT(j != nullptr);
    agent::JobState final_state = agent::JobState::Running;
    for (int i = 0; i < 60; ++i) {
        final_state = j->info().state;
        if (final_state != agent::JobState::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    agent::JobInfo info = j->info();
    // The daemon was terminated and reaped — either the reader forced it
    // (Killed) or the reap landed before the transition (Done). Never an
    // unreaped-child Done with a bogus exit code. The stop asks first now, so
    // an ordinary daemon dies of SIGTERM (143); SIGKILL (137) is the escalation
    // for a group that ignores the ask.
    ASSERT(info.state != agent::JobState::Running);
    ASSERT(info.exit_code == 128 + SIGTERM || info.exit_code == 128 + SIGKILL);
    jobs.stop(id);
}

// ---------------------------------------------------------------------------
// Crash/hang/OOM robustness cluster (top-5 item #5)
// ---------------------------------------------------------------------------

// A malicious/buggy server streams a tool-call delta with a huge sparse
// index; the parser must cap it, never allocate a billion empty slots.
TEST(sse_tool_call_index_capped) {
    agent::Message m;
    auto p = agent::make_dialect("openai")->make_decoder(m, [](const agent::StreamChunk&) {}, "");
    agent::json delta = {
        {"tool_calls",
         agent::json::array({{{"index", 100000},
                              {"id", "bomb"},
                              {"type", "function"},
                              {"function", {{"name", "search"}, {"arguments", "{}"}}}}})}};
    agent::json choice = {{"delta", delta}};
    std::string data =
        "data: " + agent::json{{"choices", agent::json::array({choice})}}.dump() + "\n\n";
    p->on_write(data.data(), 1, data.size());
    ASSERT(m.tool_calls.size() <= agent::kMaxToolCallsPerMessage);
}

// The raw stream accumulation is diagnostics-only; it must be bounded.
TEST(sse_raw_body_bounded) {
    agent::Message m;
    auto p = agent::make_dialect("openai")->make_decoder(m, [](const agent::StreamChunk&) {}, "");
    std::string junk(std::size_t(1024) * 1024, 'x');
    p->on_write(junk.data(), 1, junk.size());
    ASSERT(p->raw_body().size() <= agent::kMaxRawBodyBytes);
}

// Some gateways (kilocode routing to MiniMax et al.) stream tool-call deltas
// with 1-BASED `index` values, which the zero-based OpenAI contract does not
// use. The parser must not leave the leading placeholder slot (index 0) as an
// empty {} tool call: dispatch would log "unknown tool: " and the empty call
// would poison history replay (next request HTTP 400). Regression for the
// kilocode kilo-auto/free failures.
TEST(sse_one_based_tool_call_index_compacted) {
    agent::Message m;
    auto p = agent::make_dialect("openai")->make_decoder(m, [](const agent::StreamChunk&) {}, "");
    auto ev = [](const agent::json& tc) -> std::string {
        agent::json delta = {{"tool_calls", tc}};
        agent::json choice = {{"delta", delta}};
        return "data: " + agent::json{{"choices", agent::json::array({choice})}}.dump() + "\n\n";
    };
    // The kilocode gateway's real wire shape: index is 1, never 0.
    agent::json frag = {{"index", 1},
                        {"id", "call_1"},
                        {"type", "function"},
                        {"function", {{"name", "read"}, {"arguments", "{}"}}}};
    std::string sse = ev(agent::json::array({frag}));
    p->on_write(sse.c_str(), sse.size(), 1);
    p->finalize();

    ASSERT(m.tool_calls.is_array());
    // The sparse placeholder at index 0 must be compacted away; only the
    // real, name-bearing call at index 1 survives, reindexed to 0.
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "read");
    ASSERT_EQ(m.tool_calls[0]["id"], "call_1");
    // No element may be an empty/nameless placeholder.
    for (const auto& tc : m.tool_calls) {
        ASSERT(tc.contains("function"));
        ASSERT(tc["function"].contains("name"));
        ASSERT(!tc["function"]["name"].get<std::string>().empty());
    }
}

// A truncated stream that delivered an id but never the function name must
// not yield a dispatchable (then denied, history-poisoning) tool call: the
// id-only slot is dropped at finalize like any other incomplete call.
TEST(sse_id_only_tool_call_dropped) {
    agent::Message m;
    auto p = agent::make_dialect("openai")->make_decoder(m, [](const agent::StreamChunk&) {}, "");
    auto ev = [](const agent::json& tc) -> std::string {
        agent::json delta = {{"tool_calls", tc}};
        agent::json choice = {{"delta", delta}};
        return "data: " + agent::json{{"choices", agent::json::array({choice})}}.dump() + "\n\n";
    };
    // id + type but the function name never arrives (truncated/errored
    // stream) — this is not a call amber can execute or replay.
    agent::json frag = {{"index", 0},
                        {"id", "call_ghost"},
                        {"type", "function"},
                        {"function", {{"arguments", "{}"}}}};
    std::string sse = ev(agent::json::array({frag}));
    p->on_write(sse.c_str(), sse.size(), 1);
    p->finalize();

    ASSERT(m.tool_calls.is_null() || m.tool_calls.empty());
}

// One bad line must not discard the whole config or crash startup.
TEST(config_bad_line_skipped_rest_parsed) {
    std::string path = "/tmp/amber_cfg_badline.txt";
    {
        std::ofstream f(path);
        f << "model=\"my-model\"\n";
        f << "temperature=hot\n"; // unparseable numeric
        f << "max_tool_iterations=5\n";
    }
    agent::Config c;
    bool threw = false;
    try {
        c.load(path);
    } catch (...) {
        threw = true;
    }
    ASSERT(!threw);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_EQ(c.max_tool_iterations, 5);
    // The bad key falls back to its default and is reported.
    ASSERT(!c.warnings.empty());
    std::remove(path.c_str());
}

// One bad timeout_s in a server conf must skip that server, not all servers.
TEST(mcp_config_bad_timeout_skips_that_server) {
    std::string ws = "/tmp/amber_mcp_badtimeout_ws";
    std::string home = "/tmp/amber_mcp_badtimeout_home";
    run_cmd("rm -rf " + ws + " " + home);
    agent::Workspace::set_root(ws);
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("AMBER_MCP_SERVERS");
    run_cmd("mkdir -p " + ws + "/.amber/mcp " + home + "/.config/amber/mcp");
    {
        std::ofstream f(ws + "/.amber/mcp/good.conf");
        f << "type=stdio\ncommand=/usr/bin/good\ntimeout_s=30\n";
    }
    {
        std::ofstream f(ws + "/.amber/mcp/bad.conf");
        f << "type=stdio\ncommand=/usr/bin/bad\ntimeout_s=hot\n";
    }
    bool threw = false;
    std::map<std::string, agent::McpServerConfig> servers;
    try {
        servers = agent::load_mcp_servers();
    } catch (...) {
        threw = true;
    }
    ASSERT(!threw);
    ASSERT(servers.count("good") == 1u);
    ASSERT_EQ(servers["good"].timeout_s, 30);
}

// A named pipe must be rejected up front, not block the agent forever.
TEST(read_tool_rejects_fifo) {
    agent::Workspace::set_root("/tmp/amber_read_fifo_ws");
    run_cmd("rm -rf /tmp/amber_read_fifo_ws && mkdir -p /tmp/amber_read_fifo_ws");
    run_cmd("mkfifo /tmp/amber_read_fifo_ws/pipe.txt");
    auto tool = agent::make_read_tool();
    auto future =
        std::async(std::launch::async, [&tool]() { return tool->execute({{"path", "pipe.txt"}}); });
    ASSERT(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto r = future.get();
    ASSERT(!r.ok);
    ASSERT(!r.error.empty());
}

// A single enormous line must be truncated, not flooded into the context.
TEST(read_tool_truncates_long_lines) {
    agent::Workspace::set_root("/tmp/amber_read_longline_ws");
    run_cmd("rm -rf /tmp/amber_read_longline_ws && mkdir -p /tmp/amber_read_longline_ws");
    {
        std::ofstream f("/tmp/amber_read_longline_ws/big.txt");
        f << std::string(std::size_t(100) * 1024, 'a') << "\n";
    }
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", "big.txt"}});
    ASSERT(r.ok);
    ASSERT(r.output.size() < std::size_t(64) * 1024);
    ASSERT(r.output.find("...") != std::string::npos);
}

// Saving a session must not delete the index file (deleting it forces a full
// re-parse of every session on the next list()).
TEST(session_save_keeps_index) {
    std::string dir = "/tmp/amber_session_index_ws/sessions";
    run_cmd("rm -rf /tmp/amber_session_index_ws && mkdir -p " + dir);
    agent::Workspace::set_root("/tmp/amber_session_index_ws");
    agent::SessionStore store(dir);
    {
        std::ofstream f(dir + "/index.json");
        f << "{\"sessions\": []}\n";
    }
    agent::Session s;
    s.title = "t";
    ASSERT(store.save(s));
    ASSERT(std::filesystem::exists(dir + "/index.json"));
}

// ---------------------------------------------------------------------------
// Session brief — store, merge, parse (RED: agent::SessionBriefStore)
// Spec: docs/plan/session-brief.md
// ---------------------------------------------------------------------------

// [SB-01] The store round-trips to disk and persists across a simulated
// restart (load after save in a fresh store instance).
TEST(session_brief_store_load_save) {
    std::string path = "/tmp/amber_brief_loadsave.json";
    std::remove(path.c_str());
    agent::SessionBriefStore store;
    agent::SessionBrief b;
    b.intent = "Fix the parser crash on empty input";
    b.direction = "Add a null check in parse_body() before dereferencing";
    b.done = {"added null check", "ran unit tests"};
    b.earlier = "reproduced the crash with empty stdin";
    b.next = "add a regression test in tests/run_tests.cpp";
    b.avoid = {"rewriting parse_body from scratch — too risky"};
    store.merge(b);
    ASSERT(store.save(path));

    agent::SessionBriefStore reloaded;
    ASSERT(reloaded.load(path));
    ASSERT(!reloaded.empty());
    std::string rendered = reloaded.render();
    ASSERT(rendered.find("Fix the parser crash") != std::string::npos);
    ASSERT(rendered.find("null check") != std::string::npos);
    ASSERT(rendered.find("added null check") != std::string::npos);
    ASSERT(rendered.find("regression test") != std::string::npos);
    ASSERT(rendered.find("rewriting parse_body") != std::string::npos);
    std::remove(path.c_str());
}

// [SB-02] Merge appends new Done entries, caps at 10, and folds older into
// the earlier: line (extend, not regenerate).
TEST(session_brief_merge_done_append_and_cap) {
    agent::SessionBriefStore store;
    agent::SessionBrief b1;
    b1.done = {"step1", "step2", "step3"};
    b1.earlier = "initial work";
    store.merge(b1);

    // Merge a second brief that adds more done entries — total exceeds 10.
    agent::SessionBrief b2;
    b2.done = {"step4", "step5", "step6", "step7", "step8", "step9", "step10", "step11", "step12"};
    store.merge(b2);

    std::string rendered = store.render();
    // The last 10 done entries should be present verbatim (step3..step12).
    ASSERT(rendered.find("step12") != std::string::npos);
    ASSERT(rendered.find("step3") != std::string::npos);
    // step1 and step2 were folded into earlier: — they should NOT appear
    // as individual done entries but the earlier: line must exist.
    ASSERT(rendered.find("earlier:") != std::string::npos);
}

// [SB-03] Avoid is append-only — it never replaces; drops oldest when capped.
TEST(session_brief_merge_avoid_append_only) {
    agent::SessionBriefStore store;
    agent::SessionBrief b1;
    b1.avoid = {"approach-A — failed: too slow"};
    store.merge(b1);

    agent::SessionBrief b2;
    b2.avoid = {"approach-B — failed: breaks API"};
    store.merge(b2);

    std::string rendered = store.render();
    // Both avoid entries survive — append-only, never replaced.
    ASSERT(rendered.find("approach-A") != std::string::npos);
    ASSERT(rendered.find("approach-B") != std::string::npos);
}

// [SB-04] Intent, Direction, and Next are replaced (not appended) on merge.
TEST(session_brief_merge_intent_replace) {
    agent::SessionBriefStore store;
    agent::SessionBrief b1;
    b1.intent = "original intent";
    b1.direction = "original direction";
    b1.next = "original next";
    store.merge(b1);

    agent::SessionBrief b2;
    b2.intent = "refined intent";
    b2.direction = "refined direction";
    b2.next = "refined next";
    store.merge(b2);

    std::string rendered = store.render();
    ASSERT(rendered.find("refined intent") != std::string::npos);
    ASSERT(rendered.find("refined direction") != std::string::npos);
    ASSERT(rendered.find("refined next") != std::string::npos);
    // Original values must be gone (replace, not append).
    ASSERT(rendered.find("original intent") == std::string::npos);
    ASSERT(rendered.find("original direction") == std::string::npos);
    ASSERT(rendered.find("original next") == std::string::npos);
}

// [SB-05] parse_compression_response extracts the brief field alongside
// memories and skills. A malformed brief does not affect memories/skills.
TEST(session_brief_parse_extracts_brief) {
    std::string payload = R"({
        "classification": [{"turns": "0-0", "tag": "core", "summary": ""}],
        "memories": [{"content": "uses make", "tags": ["build"], "action": "upsert"}],
        "skills": [],
        "brief": {
            "intent": "fix the bug",
            "direction": "patch parser.c",
            "done": ["found the bug", "wrote the fix"],
            "next": "run tests",
            "avoid": ["rewriting parser — too risky"]
        }
    })";
    auto cr = agent::parse_compression_response(payload);
    ASSERT(cr.segments.size() == 1u);
    ASSERT(cr.memory_ops.size() == 1u);
    ASSERT(cr.brief.has_value());
    ASSERT(cr.brief->intent == "fix the bug");
    ASSERT(cr.brief->direction == "patch parser.c");
    ASSERT(cr.brief->done.size() == 2u);
    ASSERT(cr.brief->next == "run tests");
    ASSERT(cr.brief->avoid.size() == 1u);
}

// [SB-06] A malformed or missing brief in the extract response does not
// affect memories/skills — the brief store retains its last good state.
TEST(session_brief_parse_failure_nonfatal) {
    std::string payload = R"({
        "classification": [{"turns": "0-0", "tag": "core", "summary": ""}],
        "memories": [{"content": "uses make", "tags": ["build"], "action": "upsert"}],
        "skills": [{"content": "run tests", "trigger_phrase": "test", "action": "upsert"}]
    })";
    auto cr = agent::parse_compression_response(payload);
    // Memories and skills parsed fine.
    ASSERT(cr.memory_ops.size() == 1u);
    ASSERT(cr.skill_ops.size() == 1u);
    // Brief is absent — optional, no crash.
    ASSERT(!cr.brief.has_value());
}

// [SB-07] The rendered brief is capped at ~2 KB so re-injection stays cheap.
TEST(session_brief_size_cap) {
    agent::SessionBriefStore store;
    agent::SessionBrief b;
    b.intent = std::string(500, 'i');
    b.direction = std::string(500, 'd');
    b.next = std::string(500, 'n');
    b.done = {std::string(200, 'x'), std::string(200, 'y')};
    b.avoid = {std::string(200, 'a')};
    store.merge(b);
    std::string rendered = store.render();
    ASSERT(rendered.size() <= 2048u);
}

// ---------------------------------------------------------------------------
// M1: per-window run scope — the ambient context a worker thread installs so
// shared tools resolve the CALLING agent's cancel token and skill catalog
// rather than whichever object was bound at registration.
// ---------------------------------------------------------------------------

TEST(run_scope_run_cancelled_falls_back) {
    agent::CancellationToken host;
    ASSERT_FALSE(agent::run_cancelled(host));
    host.request();
    ASSERT_TRUE(agent::run_cancelled(host));
}

TEST(run_scope_run_cancelled_uses_scoped_token) {
    agent::CancellationToken host, scoped;
    agent::RunScope scope;
    scope.cancel_token = &scoped;
    agent::ScopedRunScope guard(scope);
    scoped.request();
    // The scoped token answers; the host fallback is untouched.
    ASSERT_TRUE(agent::run_cancelled(host));
    ASSERT_FALSE(host.is_requested());
}

TEST(run_scope_run_cancelled_walks_parent_chain) {
    // A nested (sub-agent) run is cancelled through an ancestor's token:
    // cancelling the window cancels its in-flight sub-agent too.
    agent::CancellationToken host, t_outer, t_inner;
    agent::RunScope outer, inner;
    outer.cancel_token = &t_outer;
    inner.cancel_token = &t_inner;
    agent::ScopedRunScope g1(outer);
    agent::ScopedRunScope g2(inner);
    ASSERT_FALSE(agent::run_cancelled(host));
    t_outer.request();
    ASSERT_TRUE(agent::run_cancelled(host));
}

TEST(run_scope_nested_restores_outer) {
    agent::RunScope outer, inner;
    agent::CancellationToken t_outer, t_inner;
    outer.cancel_token = &t_outer;
    inner.cancel_token = &t_inner;
    ASSERT_EQ(agent::current_run_scope(), nullptr);
    {
        agent::ScopedRunScope g1(outer);
        ASSERT(agent::current_run_scope() != nullptr);
        ASSERT_EQ(agent::current_run_scope()->leaf.cancel_token, &t_outer);
        {
            agent::ScopedRunScope g2(inner);
            ASSERT_EQ(agent::current_run_scope()->leaf.cancel_token, &t_inner);
        }
        ASSERT_EQ(agent::current_run_scope()->leaf.cancel_token, &t_outer);
    }
    ASSERT_EQ(agent::current_run_scope(), nullptr);
}

TEST(run_scope_resolves_the_session_skill_catalog) {
    agent::Config cfg;
    agent::SkillCatalog b(cfg, {}, "/tmp/amber_rs_home_b");
    // No scope installed: no session, so no catalog answers. The skill tools
    // report that instead of serving one bound at registration.
    ASSERT_EQ(agent::session_catalog(), nullptr);
    agent::RunScope scope;
    scope.skills = &b;
    agent::ScopedRunScope guard(scope);
    ASSERT_EQ(agent::session_catalog(), &b);
}

TEST(run_scope_record_activation_goes_to_agent_sink) {
    agent::ActivationSink sink;
    // Outside a run: a no-op (body still served; only bookkeeping skipped).
    agent::record_activation("x", "body");
    ASSERT_TRUE(sink.snapshot().empty());
    agent::RunScope scope;
    scope.activated = &sink;
    agent::ScopedRunScope guard(scope);
    agent::record_activation("x", "body");
    agent::record_activation("x", "body"); // dedup: already active
    agent::record_activation("y", "body2");
    auto snap = sink.snapshot();
    ASSERT_EQ(snap.size(), 2u);
    ASSERT_EQ(snap[0].name, "x");
    ASSERT_EQ(snap[1].name, "y");
}

// ---------------------------------------------------------------------------
// FIX-034 Phase 1: PromptRegistry is written by the host thread (plugin
// install/unwind) and read by the agent thread (prompt rendering), so it must
// be internally synchronized and must never invoke plugin callables while its
// lock is held.
// ---------------------------------------------------------------------------

TEST(prompt_registry_concurrent_add_remove_and_render) {
    agent::PromptRegistry reg;
    agent::Contribution stable = reg.add("core", "stable", 0, [] { return std::string("S"); });
    std::atomic<bool> missing{false};

    std::thread writer([&] {
        for (int i = 0; i < 2000; ++i) {
            agent::Contribution c = reg.add("plugin", "p", 10, [] { return std::string("P"); });
            c.remove();
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 2000; ++i) {
            auto out = reg.render_all(agent::PromptPlacement::Tail);
            bool found = false;
            for (const auto& s : out)
                if (s == "S")
                    found = true;
            if (!found)
                missing.store(true);
        }
    });
    writer.join();
    reader.join();
    stable.remove();
    ASSERT_FALSE(missing.load()); // the stable block is never removed
    ASSERT_EQ(reg.size(), 0u);
}

TEST(prompt_registry_render_does_not_hold_lock_during_callback) {
    agent::PromptRegistry reg;
    // A render callback that re-enters the registry would deadlock if the
    // callable were invoked while the registry lock is held.
    agent::Contribution c = reg.add("p", "reentrant", 0, [&reg] {
        (void)reg.size();
        (void)reg.items();
        return std::string("R");
    });
    auto out = reg.render_all(agent::PromptPlacement::Tail);
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0], "R");
    c.remove();
}

TEST(prompt_registry_size_tracks_add_and_remove) {
    agent::PromptRegistry reg;
    ASSERT_EQ(reg.size(), 0u);
    agent::Contribution a = reg.add("a", "1", 0, [] { return std::string("a"); });
    agent::Contribution b = reg.add("b", "2", 0, [] { return std::string("b"); });
    ASSERT_EQ(reg.size(), 2u);
    a.remove();
    ASSERT_EQ(reg.size(), 1u);
    b.remove();
    ASSERT_EQ(reg.size(), 0u);
}

// ---------------------------------------------------------------------------
// FIX-033 (RED): the run scope must survive the tool-dispatch thread hop.
// dispatch_tool_calls runs every approved tool on a std::async worker, so the
// worker must observe the CALLING agent's RunScope — its cancel token, skill
// catalog and activation sink — not the registration-bound fallbacks. Every
// test below fails today because the worker's t_run_scope is nullptr.
// ---------------------------------------------------------------------------

namespace {

struct ScopeObservation {
    bool saw_scope = false;
    bool cancelled = false;
    const agent::SkillCatalog* resolved_catalog = nullptr;
    // Never requested: isolates the scope chain from the fallback path, so a
    // true `cancelled` can only come from the scoped token surviving the hop.
    agent::CancellationToken inert;
    // When set, execute() waits for the cancel before observing it. A test that
    // spawns a worker and then requests the cancel has a race: whether the worker
    // sees it depends on whether it was scheduled before or after the request, and
    // under TSan the interleaving flips. Waiting makes the observation point
    // deterministic while still failing if the cancel never propagates -- the spin
    // is bounded, so a broken scope chain ends in the assertion, not a hang.
    bool wait_for_cancel = false;
};

// Reports what the run scope resolved to for THIS worker thread.
class ScopeProbeTool : public agent::Tool {
public:
    explicit ScopeProbeTool(ScopeObservation& obs) : obs_(obs) {}

    std::string name() const noexcept override { return "probe"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "run-scope probe"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", agent::json::object()}};
    }

    agent::ToolResult execute(const agent::json& args) const override {
        if (obs_.wait_for_cancel) {
            constexpr int kMaxSpins = 1000000;
            for (int spins = 0; spins < kMaxSpins && !agent::run_cancelled(obs_.inert); ++spins)
                std::this_thread::yield();
        }
        obs_.saw_scope = agent::current_run_scope() != nullptr;
        obs_.cancelled = agent::run_cancelled(obs_.inert);
        // The session catalog, resolved the way the skill tools resolve it:
        // from the running agent's scope, never from a registration binding.
        obs_.resolved_catalog = agent::session_catalog();
        agent::record_activation(skill_name(args), "body");
        agent::ToolResult r;
        r.ok = true;
        r.output = "probe";
        return r;
    }

private:
    static std::string skill_name(const agent::json& args) {
        if (args.contains("skill") && args["skill"].is_string()) {
            std::string s = args["skill"].get<std::string>();
            if (!s.empty())
                return s;
        }
        return "probe-skill";
    }
    ScopeObservation& obs_;
};

// Installs a nested scope on the worker, the way a sub-agent's Agent::run does
// inside the task tool, and reports whether an ANCESTOR token still answers.
class NestedScopeProbeTool : public agent::Tool {
public:
    explicit NestedScopeProbeTool(ScopeObservation& obs) : obs_(obs) {}

    std::string name() const noexcept override { return "probe"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "nested run-scope probe"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", agent::json::object()}};
    }

    agent::ToolResult execute(const agent::json&) const override {
        agent::RunScope inner;
        inner.cancel_token = &inner_token_; // this sub-run's own token, never requested
        agent::ScopedRunScope guard(inner);
        obs_.cancelled = agent::run_cancelled(obs_.inert);
        agent::ToolResult r;
        r.ok = true;
        r.output = "probe";
        return r;
    }

private:
    ScopeObservation& obs_;
    agent::CancellationToken inner_token_;
};

// Drive one approved probe call through the real dispatch path.
bool run_probe_dispatch(agent::ToolRegistry& reg, agent::Config& cfg,
                        const std::string& skill = {}) {
    agent::ConversationLog log;
    agent::AgentHooks hooks;
    std::set<std::string> approved;
    agent::Context ctx;
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "probe"}, {"arguments", {{"skill", skill}}}};
    agent::json calls = agent::json::array({tc});
    return agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved, nullptr, nullptr,
                                      &ctx);
}

} // namespace

TEST(dispatch_worker_sees_installed_run_scope) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    ScopeObservation obs;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<ScopeProbeTool>(obs));

    agent::CancellationToken token;
    agent::RunScope scope;
    scope.cancel_token = &token;
    agent::ScopedRunScope guard(scope);

    ASSERT(run_probe_dispatch(reg, cfg));
    ASSERT(obs.saw_scope); // worker inherited the calling thread's scope
}

TEST(dispatch_worker_cancel_reads_scoped_token) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    ScopeObservation obs;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<ScopeProbeTool>(obs));

    agent::CancellationToken token;
    token.request(); // the calling agent's run was cancelled
    agent::RunScope scope;
    scope.cancel_token = &token;
    agent::ScopedRunScope guard(scope);

    ASSERT(run_probe_dispatch(reg, cfg));
    ASSERT(obs.cancelled); // fallback is inert: only the scoped token can answer
}

TEST(dispatch_worker_resolves_scoped_catalog) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::SkillCatalog scoped(cfg, {}, "/tmp/amber_fix033_scoped3");
    ScopeObservation obs;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<ScopeProbeTool>(obs));

    agent::RunScope scope;
    scope.skills = &scoped;
    agent::ScopedRunScope guard(scope);

    ASSERT(run_probe_dispatch(reg, cfg));
    ASSERT(obs.resolved_catalog == &scoped);
}

TEST(dispatch_worker_records_activation_in_scope) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    ScopeObservation obs;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<ScopeProbeTool>(obs));

    agent::ActivationSink sink;
    agent::RunScope scope;
    scope.activated = &sink;
    agent::ScopedRunScope guard(scope);

    ASSERT(run_probe_dispatch(reg, cfg));
    auto snap = sink.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    ASSERT_EQ(snap[0].name, "probe-skill");
}

TEST(dispatch_worker_nested_scope_keeps_ancestor_token) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    ScopeObservation obs;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<NestedScopeProbeTool>(obs));

    agent::CancellationToken outer;
    outer.request(); // the window's run was cancelled
    agent::RunScope scope;
    scope.cancel_token = &outer;
    agent::ScopedRunScope guard(scope);

    ASSERT(run_probe_dispatch(reg, cfg));
    // Only the ancestor chain can answer: the inner scope's own token was never
    // requested. This is a sub-agent cancelled through its parent.
    ASSERT(obs.cancelled);
}

TEST(dispatch_sibling_scopes_do_not_cross_cancel) {
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    ScopeObservation obs_a, obs_b;
    obs_a.wait_for_cancel = true; // A must see the cancel; B must not wait for one
    agent::ToolRegistry reg_a, reg_b;
    reg_a.register_tool(std::make_unique<ScopeProbeTool>(obs_a));
    reg_b.register_tool(std::make_unique<ScopeProbeTool>(obs_b));

    agent::CancellationToken token_a, token_b;
    agent::Config cfg_a = cfg, cfg_b = cfg;
    std::thread ta([&] {
        agent::RunScope s;
        s.cancel_token = &token_a;
        agent::ScopedRunScope g(s);
        run_probe_dispatch(reg_a, cfg_a);
    });
    std::thread tb([&] {
        agent::RunScope s;
        s.cancel_token = &token_b;
        agent::ScopedRunScope g(s);
        run_probe_dispatch(reg_b, cfg_b);
    });
    token_a.request(); // cancel A only
    ta.join();
    tb.join();
    // A's probe waits for the cancel before observing it, so this no longer depends on
    // which thread the scheduler runs first.

    ASSERT(obs_a.cancelled);       // A sees its own cancel
    ASSERT_FALSE(obs_b.cancelled); // B is untouched (its fallback is inert)
}

TEST(dispatch_concurrent_workers_share_zero_activation_loss) {
    // Many concurrent workers, each a distinct activation into ONE sink: the
    // propagated scope must deliver every record, and the sink must be safe
    // once it is reachable from workers.
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ActivationSink sink;
    std::atomic<int> next{0};
    constexpr int kThreads = 8, kPerThread = 8;

    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&] {
            agent::RunScope scope;
            scope.activated = &sink;
            agent::ScopedRunScope guard(scope);
            for (int i = 0; i < kPerThread; ++i) {
                ScopeObservation obs;
                agent::ToolRegistry reg;
                reg.register_tool(std::make_unique<ScopeProbeTool>(obs));
                agent::Config local = cfg;
                run_probe_dispatch(reg, local, "skill-" + std::to_string(next.fetch_add(1)));
            }
        });
    }
    for (auto& th : pool)
        th.join();
    ASSERT_EQ(sink.snapshot().size(), static_cast<size_t>(kThreads * kPerThread));
}

// ---------------------------------------------------------------------------
// M1: per-window cancel — each window's Agent owns an independent token.
// ---------------------------------------------------------------------------

TEST(agent_request_cancel_is_per_agent) {
    // CancellationToken copies share a flag, so two agents built from one
    // cfg would cross-cancel; hosts hand each agent a config carrying its
    // own token (WindowManager does this per window). This test builds two
    // agents the way the TUI does and asserts the isolation holds.
    agent::ToolRegistry reg;
    agent::Config cfg_a, cfg_b;
    cfg_a.cancel_token = agent::CancellationToken{};
    cfg_b.cancel_token = agent::CancellationToken{};
    agent::Agent a(cfg_a, reg);
    agent::Agent b(cfg_b, reg, {}, {}, {}, {}, {}, {}, {});
    a.request_cancel();
    ASSERT_TRUE(a.config_snapshot()->cancel_token.is_requested());
    ASSERT_FALSE(b.config_snapshot()->cancel_token.is_requested());
}

// ---------------------------------------------------------------------------
// M1 /session fork — fork_from clones the conversation and run-state so both
// legs share an identical wire prefix (KV cache reuse) and then diverge
// independently.
// ---------------------------------------------------------------------------

namespace {

std::vector<agent::Message> sample_history() {
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "sys";
    agent::Message u;
    u.role = "user";
    u.content = "hello";
    agent::Message a;
    a.role = "assistant";
    a.content = "hi there";
    msgs.push_back(std::move(sys));
    msgs.push_back(std::move(u));
    msgs.push_back(std::move(a));
    return msgs;
}

} // namespace

TEST(agent_fork_copies_identical_context) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent src(cfg, reg);
    src.set_context(sample_history());
    agent::Agent dst(cfg, reg, {}, {}, {}, {}, {}, {}, {});
    dst.fork_from(src);
    auto& s = src.context().get_all();
    auto& d = dst.context().get_all();
    ASSERT_EQ(s.size(), d.size());
    for (size_t i = 0; i < s.size(); ++i) {
        ASSERT_EQ(s[i].role, d[i].role);
        ASSERT_EQ(s[i].content, d[i].content);
    }
}

TEST(agent_fork_legs_diverge_independently) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent src(cfg, reg);
    src.set_context(sample_history());
    agent::Agent dst(cfg, reg, {}, {}, {}, {}, {}, {}, {});
    dst.fork_from(src);
    // Push on the fork must not touch the parent's sealed stack.
    agent::Message m;
    m.role = "user";
    m.content = "fork-only";
    const_cast<agent::Context&>(dst.context()).push(m);
    ASSERT_EQ(dst.context().size(), src.context().size() + 1);
    ASSERT_EQ(src.context().get_all().back().content, "hi there");
}

TEST(agent_fork_copies_runtime_state) {
    agent::Config cfg;
    cfg.model = "parent-model";
    agent::ToolRegistry reg;
    agent::Agent src(cfg, reg);
    src.set_context(sample_history());
    src.meta_["origin"] = "test";
    agent::Agent dst(cfg, reg, {}, {}, {}, {}, {}, {}, {});
    dst.fork_from(src);
    // Same wire prefix requires the same model/mode/thinking — cfg follows.
    ASSERT_EQ(dst.config_snapshot()->model, "parent-model");
    ASSERT_EQ(dst.meta_["origin"], "test");
}

TEST(agent_fork_keeps_own_cancel_token) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent src(cfg, reg);
    agent::Agent dst(cfg, reg, {}, {}, {}, {}, {}, {}, {});
    dst.fork_from(src);
    // Cancelling the fork must not cancel the parent — tokens stay separate.
    dst.request_cancel();
    ASSERT_TRUE(dst.config_snapshot()->cancel_token.is_requested());
    ASSERT_FALSE(src.config_snapshot()->cancel_token.is_requested());
}

// ---------------------------------------------------------------------------
// Workspace config vs the selected provider
// ---------------------------------------------------------------------------

// A workspace amber.conf is a base layer. An explicitly selected provider owns
// the endpoint: running amber from a checkout that pins a local server must not
// redirect the provider's traffic there. Every other key in the file still
// applies; with no provider selected the file IS the endpoint, so local
// development against a project-local server keeps working.
TEST(workspace_config_never_overrides_a_selected_provider) {
    const std::string path = "/tmp/amber_workspace_base.conf";
    {
        std::ofstream f(path);
        f << "api_base=http://127.0.0.1:8081/v1\n";
        f << "temperature=0.9\n";
    }

    agent::ProviderSelection sel;
    sel.provider = test_provider("alpha", "https://alpha.test/v1");
    agent::Config cfg;
    agent::apply_selection(cfg, sel);
    ASSERT_EQ(cfg.api_base, "https://alpha.test/v1");

    cfg.load_workspace_defaults(path);
    ASSERT_EQ(cfg.api_base, "https://alpha.test/v1");
    ASSERT_EQ(cfg.temperature, 0.9);

    // No provider selected: the file supplies the endpoint.
    agent::Config bare;
    bare.load_workspace_defaults(path);
    ASSERT_EQ(bare.api_base, "http://127.0.0.1:8081/v1");
    std::remove(path.c_str());
}

// A workspace file can configure how amber behaves, never where it writes its
// diagnostics: a cloned repository must not be able to point the transcript or
// the HTTP dump at a file amber then appends to or truncates. The operator's
// own path (env/global config) survives the workspace load.
TEST(workspace_config_cannot_choose_diagnostic_output_paths) {
    const std::string path = "/tmp/amber_workspace_sinks.conf";
    {
        std::ofstream f(path);
        f << "log_path=/tmp/hijacked-transcript.jsonl\n";
        f << "debug_log=/tmp/hijacked-http.log\n";
        f << "temperature=0.9\n";
    }

    agent::Config cfg;
    cfg.log_path = "/tmp/operator-transcript.jsonl";
    cfg.debug_log = "/tmp/operator-http.log";
    cfg.load_workspace_defaults(path);
    ASSERT_EQ(cfg.log_path, "/tmp/operator-transcript.jsonl");
    ASSERT_EQ(cfg.debug_log, "/tmp/operator-http.log");
    // Everything else in the file still applies.
    ASSERT_EQ(cfg.temperature, 0.9);

    // With no operator path, the workspace still cannot install one.
    agent::Config bare;
    bare.load_workspace_defaults(path);
    ASSERT_TRUE(bare.log_path.empty());
    ASSERT_TRUE(bare.debug_log.empty());
    std::remove(path.c_str());
}
