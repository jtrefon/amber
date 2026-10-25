#include "agent/skill_install.h"
#include "agent/skill_ledger.h"
#include "tests/test_util.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct TempDir {
    explicit TempDir(const std::string& name) : path("/tmp/amber_install_" + name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string path;
};

const char* kSkillMd = "---\nname: tdd\ndescription: Test-driven development.\n---\nbody\n";

agent::SkillPayloadFile file(const std::string& path, const std::string& contents) {
    return {path, contents};
}

agent::SkillInstallRequest request(const std::string& root, const std::string& name,
                                   std::vector<agent::SkillPayloadFile> files) {
    agent::SkillInstallRequest req;
    req.name = name;
    req.dest_root = root;
    req.files = std::move(files);
    return req;
}

std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(skill_install_writes_a_tree) {
    TempDir tmp("tree");
    const auto r = agent::install_skill_files(request(
        tmp.path, "tdd", {file("SKILL.md", kSkillMd), file("references/tests.md", "tests")}));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files, 2);
    ASSERT_EQ(r.bytes, static_cast<long long>(std::string(kSkillMd).size() + 5));
    ASSERT_TRUE(fs::exists(tmp.path + "/tdd/SKILL.md"));
    ASSERT_EQ(read(tmp.path + "/tdd/references/tests.md"), std::string("tests"));
    ASSERT_TRUE(!fs::exists(tmp.path + "/.install-tdd")); // temp is gone
}

TEST(skill_install_rejects_traversal) {
    TempDir tmp("traversal");
    const auto r = agent::install_skill_files(
        request(tmp.path, "tdd", {file("SKILL.md", kSkillMd), file("../evil.md", "x")}));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(r.error.find("unsafe path") != std::string::npos);
    ASSERT_TRUE(!fs::exists(tmp.path + "/tdd"));
}

TEST(skill_install_rejects_missing_skill_md) {
    TempDir tmp("missing");
    const auto r = agent::install_skill_files(request(tmp.path, "tdd", {file("README.md", "x")}));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(r.error.find("SKILL.md") != std::string::npos);
}

TEST(skill_install_rejects_bad_frontmatter) {
    TempDir tmp("frontmatter");
    const auto r =
        agent::install_skill_files(request(tmp.path, "tdd", {file("SKILL.md", "no frontmatter")}));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(r.error.find("malformed") != std::string::npos);
}

TEST(skill_install_conflict_needs_replace) {
    TempDir tmp("conflict");
    ASSERT_TRUE(
        agent::install_skill_files(request(tmp.path, "tdd", {file("SKILL.md", kSkillMd)})).ok);
    const auto r =
        agent::install_skill_files(request(tmp.path, "tdd", {file("SKILL.md", kSkillMd)}));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(r.error.find("already installed") != std::string::npos);
}

TEST(skill_install_replace_swaps_the_tree) {
    TempDir tmp("replace");
    ASSERT_TRUE(agent::install_skill_files(
                    request(tmp.path, "tdd", {file("SKILL.md", kSkillMd), file("old.md", "o")}))
                    .ok);
    auto req = request(tmp.path, "tdd", {file("SKILL.md", kSkillMd), file("new.md", "n")});
    req.replace = true;
    ASSERT_TRUE(agent::install_skill_files(req).ok);
    ASSERT_TRUE(!fs::exists(tmp.path + "/tdd/old.md"));
    ASSERT_TRUE(fs::exists(tmp.path + "/tdd/new.md"));
}

TEST(skill_install_failure_leaves_nothing) {
    TempDir tmp("failure");
    // "a" is written as a file, then "a/b" needs "a" to be a directory: the
    // second write fails after staging started.
    const auto r = agent::install_skill_files(
        request(tmp.path, "tdd", {file("SKILL.md", kSkillMd), file("a", "x"), file("a/b", "y")}));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(!fs::exists(tmp.path + "/tdd"));
    for (const auto& entry : fs::directory_iterator(tmp.path)) {
        const std::string name = entry.path().filename().string();
        ASSERT_TRUE(name.rfind(".install-", 0) != 0);
    }
}

TEST(skill_install_caps_file_count) {
    TempDir tmp("cap");
    std::vector<agent::SkillPayloadFile> files{file("SKILL.md", kSkillMd)};
    for (int i = 0; i < 200; ++i)
        files.push_back(file("f" + std::to_string(i) + ".md", "x"));
    const auto r = agent::install_skill_files(request(tmp.path, "tdd", std::move(files)));
    ASSERT_TRUE(!r.ok);
    ASSERT_TRUE(r.error.find("number of files") != std::string::npos);
}

TEST(skill_install_safe_path_table) {
    ASSERT_TRUE(agent::is_safe_skill_path("SKILL.md"));
    ASSERT_TRUE(agent::is_safe_skill_path("references/tests.md"));
    ASSERT_TRUE(!agent::is_safe_skill_path(""));
    ASSERT_TRUE(!agent::is_safe_skill_path("/abs"));
    ASSERT_TRUE(!agent::is_safe_skill_path("../x"));
    ASSERT_TRUE(!agent::is_safe_skill_path("a/../b"));
    ASSERT_TRUE(!agent::is_safe_skill_path("a//b"));
    ASSERT_TRUE(!agent::is_safe_skill_path("a\\b"));
    ASSERT_TRUE(!agent::is_safe_skill_path("C:/x"));
}

TEST(skill_ledger_round_trip) {
    TempDir tmp("ledger");
    const std::string path = tmp.path + "/.lock.json";
    agent::LedgerEntry e;
    e.id = "acme/skills/tdd";
    e.source = "acme/skills";
    e.slug = "tdd";
    e.ref = "main";
    e.hash = "abc";
    e.category = "engineering";
    e.installed_at_ms = 123;
    e.files = 2;
    e.bytes = 42;
    e.enabled = true;

    {
        agent::SkillLedger ledger(path);
        ledger.load();
        ASSERT_TRUE(ledger.upsert("tdd", e));
    }
    agent::SkillLedger reloaded(path);
    reloaded.load();
    const auto found = reloaded.find("tdd");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found->id, std::string("acme/skills/tdd"));
    ASSERT_EQ(found->hash, std::string("abc"));
    ASSERT_EQ(found->files, 2);
    ASSERT_EQ(found->bytes, 42);

    ASSERT_TRUE(reloaded.remove("tdd"));
    ASSERT_TRUE(!reloaded.find("tdd").has_value());
}

TEST(skill_ledger_tolerates_corrupt_file) {
    TempDir tmp("ledger_corrupt");
    const std::string path = tmp.path + "/.lock.json";
    {
        std::ofstream f(path);
        f << "not json";
    }
    agent::SkillLedger ledger(path);
    ledger.load();
    ASSERT_EQ(static_cast<int>(ledger.entries().size()), 0);
}
