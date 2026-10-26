#include "agent/skill_cli.h"

#include "tests/test_util.h"

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
