
#include <fstream>
#include <sstream>
#include <string>

#include "agent/config.h"
#include "agent/skill_file.h"
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
    std::ofstream f(path);
    f << contents;
}

void write_skill(const std::string& dir, const std::string& name,
                 const std::string& frontmatter) {
    run_cmd("mkdir -p " + dir + "/" + name);
    write_file(dir + "/" + name + "/SKILL.md", frontmatter);
}

} // namespace

// SF-01: valid frontmatter parses name/description; body is everything after
// the closing ---.
TEST(skill_parse_basic_frontmatter) {
    std::string doc =
        "---\n"
        "name: deploy\n"
        "description: Deploy the app\n"
        "---\n"
        "## Instructions\n"
        "Run make deploy.\n";
    auto meta = agent::parse_skill_meta(doc);
    ASSERT(meta.has_value());
    ASSERT_EQ(meta->name, "deploy");
    ASSERT_EQ(meta->description, "Deploy the app");
    ASSERT(meta->body.find("Run make deploy.") != std::string::npos);
}

// SF-02: '>' folded values are joined onto one line.
TEST(skill_parse_folded_description) {
    std::string doc =
        "---\n"
        "name: fold-me\n"
        "description: >\n"
        "  First line\n"
        "  Second line\n"
        "---\n"
        "body\n";
    auto meta = agent::parse_skill_meta(doc);
    ASSERT(meta.has_value());
    ASSERT_EQ(meta->description, "First line Second line");
}

// SF-07: metadata subkeys collect into a JSON object; unknown top-level keys
// are ignored (forward compatibility).
TEST(skill_parse_metadata_and_unknown_keys) {
    std::string doc =
        "---\n"
        "name: x\n"
        "description: d\n"
        "metadata:\n"
        "  author: someone\n"
        "  version: 3\n"
        "future_key: ignored\n"
        "---\n";
    auto meta = agent::parse_skill_meta(doc);
    ASSERT(meta.has_value());
    ASSERT_EQ(meta->metadata.value("author", ""), "someone");
    ASSERT_EQ(meta->metadata.value("version", ""), "3");
}

// SF-07: inline {k: v} metadata flow map also works.
TEST(skill_parse_inline_flow_metadata) {
    std::string doc =
        "---\n"
        "name: x\n"
        "description: d\n"
        "metadata: {author: alice, category: ops}\n"
        "---\n";
    auto meta = agent::parse_skill_meta(doc);
    ASSERT(meta.has_value());
    ASSERT_EQ(meta->metadata.value("author", ""), "alice");
    ASSERT_EQ(meta->metadata.value("category", ""), "ops");
}

// SF-05: a file with no frontmatter block is not a skill.
TEST(skill_parse_no_frontmatter_is_null) {
    std::string doc = "plain body, no frontmatter\n";
    ASSERT_FALSE(agent::parse_skill_meta(doc).has_value());
}

// SF-04: directory name is the canonical skill name even when the frontmatter
// disagrees.
TEST(skill_scan_dirname_wins) {
    std::string root = "/tmp/amber_sk3_scan";
    run_cmd("rm -rf " + root);
    write_skill(root, "alpha", "---\nname: wrong\n---\nbody\n");
    auto files = agent::scan_skill_dir(root, agent::SkillScope::Project);
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files[0].name, "alpha");
    ASSERT(files[0].scope == agent::SkillScope::Project);
}

// SF-02/SF-03: missing root and non-skill entries are skipped without error.
TEST(skill_scan_missing_root_empty) {
    std::string root = "/tmp/amber_sk3_scan_missing";
    run_cmd("rm -rf " + root);
    ASSERT_TRUE(agent::scan_skill_dir(root, agent::SkillScope::Project).empty());
}

TEST(skill_scan_skips_invalid_entries) {
    std::string root = "/tmp/amber_sk3_scan_invalid";
    run_cmd("rm -rf " + root);
    write_skill(root, "valid", "---\ndescription: ok\n---\nbody\n");
    write_skill(root, "Not Kebab", "---\ndescription: bad name\n---\n");
    write_skill(root, "no_meta", "just text, no frontmatter");
    write_file(root + "/loose.txt", "not a dir");
    auto files = agent::scan_skill_dir(root, agent::SkillScope::Project);
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files[0].name, "valid");
}

// SF-06: project root shadows the global root; interop is skipped unless
// enabled; first occurrence of a name wins.
TEST(skill_scan_precedence_and_interop_gate) {
    std::string ws = "/tmp/amber_sk3_ws";
    std::string global = "/tmp/amber_sk3_global";
    run_cmd("rm -rf " + ws + " " + global);
    write_skill(ws + "/.amber/skills", "dup", "---\ndescription: project\n---\n");
    write_skill(global + "/skills", "dup", "---\ndescription: global\n---\n");
    write_skill(global + "/skills", "global-only",
                "---\ndescription: g\n---\n");
    write_skill(ws + "/.claude/skills", "claude-skill",
                "---\ndescription: c\n---\n");

    agent::SkillScanPaths paths;
    paths.project = ws + "/.amber/skills";
    paths.global = global + "/skills";
    paths.claude = ws + "/.claude/skills";
    paths.codex = ws + "/.codex/skills";

    auto without_interop = agent::scan_skills(paths, false);
    ASSERT_EQ(without_interop.size(), 2u);
    for (const auto& f : without_interop) {
        ASSERT_FALSE(f.name == "claude-skill");
        if (f.name == "dup") {
            ASSERT(f.scope == agent::SkillScope::Project);
        }
    }

    auto with_interop = agent::scan_skills(paths, true);
    ASSERT_EQ(with_interop.size(), 3u);
    for (const auto& f : with_interop) {
        if (f.name == "dup") ASSERT(f.scope == agent::SkillScope::Project);
        if (f.name == "claude-skill") {
            ASSERT(f.scope == agent::SkillScope::Interop);
        }
    }
}

// SK-02: skill names must match ^[a-z0-9-]+$.
TEST(skill_is_kebab_name) {
    ASSERT_TRUE(agent::is_kebab_name("deploy"));
    ASSERT_TRUE(agent::is_kebab_name("git-commit-helper"));
    ASSERT_TRUE(agent::is_kebab_name("my2fa"));
    ASSERT_FALSE(agent::is_kebab_name(""));
    ASSERT_FALSE(agent::is_kebab_name("BadName"));
    ASSERT_FALSE(agent::is_kebab_name("has_underscore"));
    ASSERT_FALSE(agent::is_kebab_name("has space"));
}

// Default scan paths resolve relative to the workspace and config dir.
TEST(skill_default_scan_paths) {
    agent::Workspace::set_root("/tmp/amber_sk3_wsdef");
    run_cmd("rm -rf /tmp/amber_sk3_home");
    setenv("HOME", "/tmp/amber_sk3_home", 1);
    unsetenv("XDG_CONFIG_HOME");
    auto paths = agent::default_scan_paths();
    ASSERT_EQ(paths.project, "/tmp/amber_sk3_wsdef/.amber/skills");
    ASSERT_EQ(paths.global, "/tmp/amber_sk3_home/.config/amber/skills");
    ASSERT_EQ(paths.claude, "/tmp/amber_sk3_wsdef/.claude/skills");
    ASSERT_EQ(paths.codex, "/tmp/amber_sk3_wsdef/.codex/skills");
}

// Config round-trips the skills_* keys.
TEST(skill_config_keys_roundtrip) {
    std::string path = "/tmp/amber_sk3_cfg.conf";
    {
        std::ofstream f(path);
        f << "skills_interop=1\n";
        f << "skills_max_discovery=7\n";
        f << "skills_body_budget_tokens=3000\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_TRUE(c.skills_interop);
    ASSERT_EQ(c.skills_max_discovery, 7);
    ASSERT_EQ(c.skills_body_budget_tokens, 3000);
    std::remove(path.c_str());
}
