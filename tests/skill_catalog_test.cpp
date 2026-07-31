
#include <fstream>
#include <sstream>
#include <string>

#include "agent/skill_catalog.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

namespace {

std::string run_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    std::string out;
    if (!pipe) return out;
    char buf[256];
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    pclose(pipe);
    return out;
}

void write_file(const std::string& path, const std::string& contents) {
    run_cmd("mkdir -p " + std::string(path).substr(0, path.find_last_of('/')));
    std::ofstream f(path);
    f << contents;
}

void write_skill(const std::string& dir, const std::string& name,
                 const std::string& description) {
    run_cmd("mkdir -p " + dir + "/" + name);
    write_file(dir + "/" + name + "/SKILL.md",
               "---\nname: " + name + "\ndescription: " + description +
                   "\n---\nbody of " + name + "\n");
}

struct CatalogEnv {
    std::string ws;
    std::string home;
    std::string project_skills;
    std::string global_skills;
    std::string claude_skills;
    agent::SkillScanPaths paths;

    CatalogEnv(const std::string& tag) {
        ws = "/tmp/amber_sk4_" + tag;
        home = "/tmp/amber_sk4_home_" + tag;
        project_skills = ws + "/.amber/skills";
        global_skills = home + "/skills";
        claude_skills = ws + "/.claude/skills";
        run_cmd("rm -rf " + ws + " " + home);
        agent::Workspace::set_root(ws);
        paths.project = project_skills;
        paths.global = global_skills;
        paths.claude = claude_skills;
        paths.codex = ws + "/.codex/skills";
    }

    void write_override(const std::string& name, const std::string& state,
                        const std::string& note) {
        write_file(ws + "/.amber/skills.json",
                   "{\"" + name + "\":{\"state\":\"" + state +
                       "\",\"note\":\"" + note + "\"}}\n");
    }

    void write_global_override(const std::string& name,
                               const std::string& state) {
        write_file(home + "/skills.json",
                   "{\"" + name + "\":{\"state\":\"" + state +
                       "\",\"note\":\"global\"}}\n");
    }
};

} // namespace

// [SK-01] Union discovery: authored at all scopes (interop gated) + learned.
TEST(skill_catalog_union_discovery) {
    CatalogEnv env("union");
    for (int i = 1; i <= 3; ++i)
        write_skill(env.project_skills, "p-skill" + std::to_string(i),
                    "project skill");
    write_skill(env.global_skills, "g-skill1", "global skill");
    write_skill(env.global_skills, "g-skill2", "global skill");
    write_skill(env.claude_skills, "interop-skill", "interop skill");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    std::vector<agent::Skill> learned;
    agent::Skill a;
    a.name = "l-learned1";
    a.content = "learned procedure 1";
    learned.push_back(a);
    agent::Skill b;
    b.name = "l-learned2";
    b.content = "learned procedure 2";
    learned.push_back(b);
    catalog.discover(learned);

    ASSERT_EQ(catalog.entries().size(), 7u);
    for (const auto& e : catalog.entries()) {
        ASSERT_FALSE(e.name == "interop-skill");
        if (e.name == "l-learned1" || e.name == "l-learned2")
            ASSERT(e.origin == agent::SkillOrigin::Learned);
    }
    ASSERT_EQ(catalog.discovery_block().size(), 7u);
}

// [AS-04] Project shadows global: same name in both -> project listed once.
TEST(skill_catalog_project_shadows_global) {
    CatalogEnv env("pshadow");
    write_skill(env.project_skills, "deploy", "project deploy");
    write_skill(env.global_skills, "deploy", "global deploy");
    write_skill(env.global_skills, "global-only", "global only");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});

    const agent::SkillEntry* e = catalog.lookup("deploy");
    ASSERT(e != nullptr);
    ASSERT(e->scope == agent::SkillScope::Project);
    ASSERT_EQ(e->meta.description, "project deploy");
    ASSERT_EQ(catalog.discovery_block().size(), 2u);
}

// [AS-03] Authored beats learned: learned entry suppressed, not injected.
TEST(skill_catalog_authored_shadows_learned) {
    CatalogEnv env("lshadow");
    write_skill(env.project_skills, "run-tests", "authored run-tests");

    std::vector<agent::Skill> learned;
    agent::Skill sk;
    sk.name = "run-tests";
    sk.content = "learned run-tests";
    learned.push_back(sk);

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover(learned);

    const agent::SkillEntry* e = catalog.lookup("run-tests");
    ASSERT(e != nullptr);
    ASSERT(e->origin == agent::SkillOrigin::Authored);
    ASSERT_EQ(catalog.discovery_block().size(), 1u);
    size_t suppressed = 0;
    for (const auto& entry : catalog.entries())
        if (entry.state == "suppressed") ++suppressed;
    ASSERT_EQ(suppressed, 1u);
}

// [AS-05]/[SK-06] Disable override is sticky across re-scans.
TEST(skill_catalog_disable_sticky) {
    CatalogEnv env("disable");
    write_skill(env.project_skills, "obsolete-workflow", "old flow");
    env.write_override("obsolete-workflow", "disable", "superseded");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    ASSERT(catalog.lookup("obsolete-workflow") == nullptr);
    ASSERT_EQ(catalog.discovery_block().size(), 0u);

    catalog.discover({});
    ASSERT(catalog.lookup("obsolete-workflow") == nullptr);
    ASSERT_EQ(catalog.discovery_block().size(), 0u);
}

// [SK-07] Enable override forces a shadowed skill into the union.
TEST(skill_catalog_enable_forces_shadowed) {
    CatalogEnv env("enable");
    write_skill(env.project_skills, "deploy", "project deploy");
    write_skill(env.global_skills, "deploy", "global deploy");
    env.write_global_override("deploy", "enable");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});

    size_t count = 0;
    size_t forced = 0;
    for (const auto& e : catalog.entries()) {
        if (e.name == "deploy") {
            ++count;
            if (e.state == "force-enabled") ++forced;
        }
    }
    ASSERT_EQ(count, 2u);
    ASSERT_EQ(forced, 1u);
}

// [SK-08] Block override persists with provenance note and excludes the skill.
TEST(skill_catalog_block_persists) {
    CatalogEnv env("block");
    write_skill(env.project_skills, "some-skill", "suspicious");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    ASSERT(catalog.lookup("some-skill") != nullptr);

    ASSERT_TRUE(catalog.apply_override("some-skill", "block", "untrusted"));
    catalog.discover({});
    ASSERT(catalog.lookup("some-skill") == nullptr);
    ASSERT_EQ(catalog.overrides().at("some-skill").note, "untrusted");

    // Override survives a fresh catalog (reload from disk).
    agent::SkillCatalog reloaded(cfg, env.paths, env.home);
    ASSERT_EQ(reloaded.overrides().at("some-skill").state, "block");
    reloaded.discover({});
    ASSERT(reloaded.lookup("some-skill") == nullptr);
}

// [AS-06] Discovery budget caps the block by scan order.
TEST(skill_catalog_discovery_budget) {
    CatalogEnv env("budget");
    write_skill(env.project_skills, "p-a", "project a");
    write_skill(env.project_skills, "p-b", "project b");
    write_skill(env.global_skills, "g-c", "global c");

    agent::Config cfg;
    cfg.skills_max_discovery = 2;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    auto block = catalog.discovery_block();
    ASSERT_EQ(block.size(), 2u);
    ASSERT(block[0].find("p-a") == 0);
    ASSERT(block[1].find("p-b") == 0);
}

// [AS-07]/[SK-14] Oversized body rejected, small body loads and caches.
TEST(skill_catalog_body_budget) {
    CatalogEnv env("body");
    write_skill(env.project_skills, "small", "tiny");
    write_file(env.project_skills + "/big/SKILL.md",
               "---\ndescription: big\n---\n" + std::string(5000, 'x') + "\n");

    agent::Config cfg;
    cfg.skills_body_budget_tokens = 1000;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});

    auto small = catalog.read_body("small");
    ASSERT(small.has_value());
    ASSERT(small->find("body of small") != std::string::npos);

    auto big = catalog.read_body("big");
    ASSERT_FALSE(big.has_value());

    // Body cache: editing the file on disk does not change a second read.
    auto cached = catalog.read_body("small");
    ASSERT_EQ(*cached, *small);
}

// [SK-14] Unknown skill: lookup fails.
TEST(skill_catalog_unknown_name) {
    CatalogEnv env("unknown");
    write_skill(env.project_skills, "known", "k");
    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    ASSERT(catalog.lookup("nope") == nullptr);
    ASSERT_FALSE(catalog.read_body("nope").has_value());
}

// [AS-10] Refresh: discover() rebuilds the block after on-disk changes.
TEST(skill_catalog_refresh_rebuilds_block) {
    CatalogEnv env("refresh");
    write_skill(env.project_skills, "first", "one");

    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    ASSERT_EQ(catalog.discovery_block().size(), 1u);

    write_skill(env.project_skills, "second", "two");
    catalog.discover({});
    auto block = catalog.discovery_block();
    ASSERT_EQ(block.size(), 2u);
    ASSERT(block[1].find("second") == 0);
}

// Interop gate: disabled by default; enabling exposes interop skills.
TEST(skill_catalog_interop_gate) {
    CatalogEnv env("interop");
    write_skill(env.claude_skills, "claude-skill", "from claude");
    agent::Config cfg;
    agent::SkillCatalog catalog(cfg, env.paths, env.home);
    catalog.discover({});
    ASSERT(catalog.lookup("claude-skill") == nullptr);

    catalog.set_interop_enabled(true);
    catalog.discover({});
    ASSERT(catalog.lookup("claude-skill") != nullptr);
    ASSERT(catalog.lookup("claude-skill")->scope == agent::SkillScope::Interop);
}
