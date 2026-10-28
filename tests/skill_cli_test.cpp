#include "agent/skill_cli.h"
#include "agent/workspace.h"

#include "tests/test_util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <string>
#include <vector>

namespace {

agent::SkillPage page() {
    agent::SkillPage p;
    p.total = 7;
    p.cached = true;
    p.skills.push_back({"acme/skills/tdd", "tdd", "acme/skills", 1234, false});
    p.skills.push_back({"acme/skills/lint", "lint", "acme/skills", 99, false});
    return p;
}

agent::SkillDetail detail() {
    agent::SkillDetail d;
    d.ref = {"acme/skills/tdd", "tdd", "acme/skills", 1234, false};
    d.description = "Test-driven development.";
    d.license = "MIT";
    d.category = "engineering";
    d.hash = "sha-1";
    d.body = std::string(400, 'x');
    d.assets.push_back({"SKILL.md", 64, "sha-1"});
    return d;
}

agent::InstalledPackage package(const std::string& name, const std::string& scope) {
    agent::InstalledPackage p;
    p.name = name;
    p.scope = scope;
    p.id = "acme/skills/" + name;
    p.source = "acme/skills";
    p.ref = "main";
    p.hash = "sha-1";
    p.state = "enabled";
    p.files = 2;
    p.bytes = 1024;
    return p;
}

} // namespace

TEST(skill_cli_parses_every_verb_and_flag) {
    auto search = agent::parse_skill_cli({"search", "tdd", "--limit", "5", "--json"});
    ASSERT_TRUE(search.verb == agent::SkillVerb::Search);
    ASSERT_EQ(search.target, std::string("tdd"));
    ASSERT_EQ(search.limit, 5);
    ASSERT_TRUE(search.json);

    auto install = agent::parse_skill_cli({"install", "acme/skills/tdd", "--global", "--yes"});
    ASSERT_TRUE(install.verb == agent::SkillVerb::Install);
    ASSERT_TRUE(install.global);
    ASSERT_TRUE(install.yes);

    auto update = agent::parse_skill_cli({"update", "--all", "--check", "--json"});
    ASSERT_TRUE(update.verb == agent::SkillVerb::Update);
    ASSERT_TRUE(update.all);
    ASSERT_TRUE(update.check);

    ASSERT_TRUE(agent::parse_skill_cli({"list"}).verb == agent::SkillVerb::List);
    ASSERT_TRUE(agent::parse_skill_cli({"remove", "tdd"}).verb == agent::SkillVerb::Remove);
    ASSERT_TRUE(agent::parse_skill_cli({"sources", "--refresh"}).refresh);
}

TEST(skill_cli_rejects_bad_usage) {
    ASSERT_FALSE(agent::parse_skill_cli({}).error.empty());
    ASSERT_FALSE(agent::parse_skill_cli({"bogus"}).error.empty());
    ASSERT_FALSE(agent::parse_skill_cli({"search"}).error.empty());
    ASSERT_FALSE(agent::parse_skill_cli({"list", "--nope"}).error.empty());
    ASSERT_FALSE(agent::parse_skill_cli({"list", "extra", "words"}).error.empty());
}

TEST(skill_cli_usage_error_exits_two) {
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(agent::run_skill_cli({}, out, err), 2);
    ASSERT_TRUE(err.str().find("usage:") != std::string::npos);
    ASSERT_TRUE(out.str().empty());
}

TEST(skill_cli_search_renders_text_and_json) {
    const std::string text = agent::skill_search_text(page());
    ASSERT_TRUE(text.find("tdd\tacme/skills\t1234") != std::string::npos);
    ASSERT_TRUE(text.find("2 of 7 results") != std::string::npos);
    ASSERT_TRUE(text.find("(cached)") != std::string::npos);

    const std::string json = agent::skill_search_json(page());
    ASSERT_TRUE(json.find("\"count\": 7") != std::string::npos);
    ASSERT_TRUE(json.find("\"cached\": true") != std::string::npos);
    ASSERT_TRUE(json.find("\"id\": \"acme/skills/tdd\"") != std::string::npos);
}

TEST(skill_cli_detail_is_honest_about_audits) {
    const std::string text = agent::skill_detail_text(detail());
    ASSERT_TRUE(text.find("Test-driven development.") != std::string::npos);
    ASSERT_TRUE(text.find("not audited") != std::string::npos);
    ASSERT_TRUE(text.find("1 files") != std::string::npos);
    ASSERT_TRUE(text.find("MIT") != std::string::npos);

    const std::string json = agent::skill_detail_json(detail());
    ASSERT_TRUE(json.find("\"audited\": false") != std::string::npos);
    ASSERT_TRUE(json.find("\"tokens\": 100") != std::string::npos);
    ASSERT_TRUE(json.find("\"category\": \"engineering\"") != std::string::npos);
}

TEST(skill_cli_list_renders_scopes_and_state) {
    const std::vector<agent::InstalledPackage> packages = {package("tdd", "project"),
                                                           package("lint", "global")};
    const std::string text = agent::skill_list_text(packages);
    ASSERT_TRUE(text.find("project · tdd · acme/skills/tdd · enabled") != std::string::npos);
    ASSERT_TRUE(text.find("global · lint") != std::string::npos);

    const std::string json = agent::skill_list_json(packages);
    ASSERT_TRUE(json.find("\"count\": 2") != std::string::npos);
    ASSERT_TRUE(json.find("\"scope\": \"global\"") != std::string::npos);

    ASSERT_EQ(agent::skill_list_text({}), std::string("no installed skills\n"));
}

TEST(skill_cli_list_json_empty_is_an_empty_array) {
    const std::string json = agent::skill_list_json({});
    ASSERT_TRUE(json.find("\"count\": 0") != std::string::npos);
    ASSERT_TRUE(json.find("\"skills\": []") != std::string::npos);
}

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A private project root and XDG roots, restored on exit: the CLI reads the
// workspace, the global config dir and the cache.
struct CliSandbox {
    CliSandbox() {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "amber_skill_cli_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        path = mkdtemp(buf.data());
        capture("XDG_CONFIG_HOME", saved_config);
        capture("XDG_STATE_HOME", saved_state);
        capture("XDG_CACHE_HOME", saved_cache);
        setenv("XDG_CONFIG_HOME", (path + "/config").c_str(), 1);
        setenv("XDG_STATE_HOME", (path + "/state").c_str(), 1);
        setenv("XDG_CACHE_HOME", (path + "/cache").c_str(), 1);
    }
    ~CliSandbox() {
        restore("XDG_CONFIG_HOME", saved_config);
        restore("XDG_STATE_HOME", saved_state);
        restore("XDG_CACHE_HOME", saved_cache);
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    CliSandbox(const CliSandbox&) = delete;
    CliSandbox& operator=(const CliSandbox&) = delete;

    std::string path;
    std::string saved_config;
    std::string saved_state;
    std::string saved_cache;

private:
    static void capture(const char* name, std::string& out) {
        if (const char* value = std::getenv(name))
            out = value;
    }
    static void restore(const char* name, const std::string& value) {
        if (value.empty())
            unsetenv(name);
        else
            setenv(name, value.c_str(), 1);
    }
};

agent::SkillCliBackends fake_backends(std::string& body, std::string& sha) {
    agent::SkillCliBackends backends;
    backends.search = [](const std::string& query, int) {
        agent::SkillPage page;
        page.total = 1;
        page.skills.push_back({"acme/skills/tdd", "tdd", "acme/skills", 10, false});
        (void)query;
        return agent::IndexResult{page, {}};
    };
    backends.detail = [&body, &sha](const agent::SkillRef& ref) {
        agent::SkillDetail detail;
        detail.ref = ref;
        detail.description = "Test-driven.";
        detail.license = "MIT";
        detail.default_branch = "main";
        detail.skill_dir = "skills/tdd";
        detail.hash = sha;
        detail.body = body;
        detail.assets.push_back({"SKILL.md", body.size(), sha});
        return agent::DetailResult{detail, {}};
    };
    backends.assets = [&body, &sha](const agent::SkillDetail&) {
        std::vector<agent::SkillAssetContent> files{{"SKILL.md", body}};
        (void)sha;
        return agent::AssetsResult{files, {}};
    };
    backends.install = [](const agent::SkillInstallRequest& request) {
        return agent::install_skill_files(request);
    };
    return backends;
}

} // namespace

TEST(skill_cli_end_to_end_against_injected_backends) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    std::string body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody one\n";
    std::string sha = "sha-1";
    const agent::SkillCliBackends backends = fake_backends(body, sha);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(agent::run_skill_cli({"search", "tdd"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("1 of 1 results") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd"}, out, err, backends), 1);
    ASSERT_TRUE(err.str().find("refusing to install") != std::string::npos);

    err.str("");
    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd", "--yes"}, out, err, backends), 0);
    const std::string skill_md = sandbox.path + "/.amber/skills/tdd/SKILL.md";
    ASSERT_TRUE(read_file(skill_md).find("body one") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"list"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("project · tdd · acme/skills/tdd") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"show", "tdd", "--json"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("\"name\": \"tdd\"") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"update", "--check"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("up to date") != std::string::npos);

    // A second revision upstream is offered, then applied atomically.
    sha = "sha-2";
    body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody two\n";
    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"update", "--check"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("update available") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"update"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("updated") != std::string::npos);
    ASSERT_TRUE(read_file(skill_md).find("body two") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"sources", "--json"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("\"cache\"") != std::string::npos);

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"remove", "tdd"}, out, err, backends), 0);
    ASSERT_TRUE(!std::filesystem::exists(sandbox.path + "/.amber/skills/tdd"));
}

namespace {

std::string global_skill_dir(const CliSandbox& sandbox, const std::string& name) {
    return sandbox.path + "/config/amber/skills/" + name;
}

} // namespace

TEST(skill_cli_honours_the_default_install_scope_and_project_flag) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    setenv("AMBER_SKILLS_INSTALL_SCOPE", "global", 1);
    std::string body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody\n";
    std::string sha = "sha-1";
    const agent::SkillCliBackends backends = fake_backends(body, sha);
    std::ostringstream out;
    std::ostringstream err;

    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd", "--yes"}, out, err, backends), 0);
    ASSERT_TRUE(std::filesystem::exists(global_skill_dir(sandbox, "tdd") + "/SKILL.md"));
    ASSERT_TRUE(!std::filesystem::exists(sandbox.path + "/.amber/skills/tdd"));

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"remove", "tdd"}, out, err, backends), 0);
    ASSERT_TRUE(!std::filesystem::exists(global_skill_dir(sandbox, "tdd")));

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd", "--yes", "--project"}, out, err,
                                   backends),
              0);
    ASSERT_TRUE(std::filesystem::exists(sandbox.path + "/.amber/skills/tdd/SKILL.md"));
    unsetenv("AMBER_SKILLS_INSTALL_SCOPE");
}

TEST(skill_cli_index_none_disables_search) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    setenv("AMBER_SKILLS_INDEX", "none", 1);
    unsetenv("AMBER_SKILLS_INDEX_URL");
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(agent::run_skill_cli({"search", "tdd"}, out, err), 1);
    ASSERT_TRUE(err.str().find("disabled") != std::string::npos);
    unsetenv("AMBER_SKILLS_INDEX");
}

TEST(skill_cli_update_diff_shows_the_change_and_writes_nothing) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    std::string body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody one\n";
    std::string sha = "sha-1";
    const agent::SkillCliBackends backends = fake_backends(body, sha);
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd", "--yes"}, out, err, backends), 0);
    const std::string skill_md = sandbox.path + "/.amber/skills/tdd/SKILL.md";

    sha = "sha-2";
    body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody two\n";
    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"update", "--diff"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("-body one") != std::string::npos);
    ASSERT_TRUE(out.str().find("+body two") != std::string::npos);
    ASSERT_TRUE(read_file(skill_md).find("body one") != std::string::npos); // nothing written

    out.str("");
    ASSERT_EQ(agent::run_skill_cli({"update", "--check"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("update available: tdd:") != std::string::npos);
}

TEST(skill_cli_update_refuses_local_edits_until_forced) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    std::string body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody one\n";
    std::string sha = "sha-1";
    const agent::SkillCliBackends backends = fake_backends(body, sha);
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(agent::run_skill_cli({"install", "acme/skills/tdd", "--yes"}, out, err, backends), 0);
    const std::string skill_md = sandbox.path + "/.amber/skills/tdd/SKILL.md";
    {
        std::ofstream f(skill_md, std::ios::app);
        f << "my own notes\n";
    }

    sha = "sha-2";
    body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody two\n";
    out.str("");
    err.str("");
    ASSERT_EQ(agent::run_skill_cli({"update"}, out, err, backends), 1);
    ASSERT_TRUE(out.str().find("locally modified") != std::string::npos);
    ASSERT_TRUE(read_file(skill_md).find("my own notes") != std::string::npos);

    out.str("");
    err.str("");
    ASSERT_EQ(agent::run_skill_cli({"update", "--force"}, out, err, backends), 0);
    ASSERT_TRUE(out.str().find("local edits overridden") != std::string::npos);
    ASSERT_TRUE(read_file(skill_md).find("body two") != std::string::npos);
    ASSERT_TRUE(read_file(skill_md).find("my own notes") == std::string::npos);
}

TEST(skill_cli_update_checks_can_be_disabled) {
    CliSandbox sandbox;
    agent::ScopedProjectRoot project(sandbox.path);
    setenv("AMBER_SKILLS_UPDATE_CHECK", "never", 1);
    std::string body = "---\nname: tdd\ndescription: Test-driven.\n---\nbody one\n";
    std::string sha = "sha-1";
    const agent::SkillCliBackends backends = fake_backends(body, sha);
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(agent::run_skill_cli({"update", "--check"}, out, err, backends), 1);
    ASSERT_TRUE(err.str().find("disabled") != std::string::npos);
    unsetenv("AMBER_SKILLS_UPDATE_CHECK");
}
