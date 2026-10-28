// What an update would do: the SKILL.md diff, added and removed files, and the
// local-edit flag that decides whether applying needs an explicit override.

#include "agent/skill_update_review.h"

#include "agent/skill_install.h"

#include "test_util.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace agent;

namespace {

namespace fs = std::filesystem;

LedgerEntry entry_for(const std::string& body, const std::string& hash = "aaa1111") {
    LedgerEntry entry;
    entry.id = "acme/skills/tdd";
    entry.source = "acme/skills";
    entry.slug = "tdd";
    entry.hash = hash;
    entry.local_hash = skill_content_hash(body);
    return entry;
}

SkillDetail detail_for(const std::string& body, std::vector<std::string> assets,
                       const std::string& hash = "bbb2222") {
    SkillDetail detail;
    detail.ref.id = "acme/skills/tdd";
    detail.ref.name = "tdd";
    detail.ref.source = "acme/skills";
    detail.hash = hash;
    detail.body = body;
    for (auto& path : assets)
        detail.assets.push_back({path, 10, "sha"});
    return detail;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST(update_review_diffs_the_body_and_counts_lines) {
    const std::string installed = "# TDD\nWrite the test first.\nRun it.\n";
    const std::string remote = "# TDD\nWrite the test first.\nWatch it fail.\nThen implement.\n";
    const UpdateReview review =
        build_update_review(entry_for(installed), detail_for(remote, {"SKILL.md"}), installed, {});
    ASSERT_TRUE(review.body_changed);
    ASSERT_FALSE(review.local_modified);
    ASSERT_EQ(review.body_counts.removed, 1);
    ASSERT_EQ(review.body_counts.added, 2);
    ASSERT_TRUE(review.status.find("+2 -1 lines") != std::string::npos);
    ASSERT_TRUE(review.status.find("aaa1111") != std::string::npos);
    ASSERT_TRUE(review.status.find("bbb2222") != std::string::npos);
}

TEST(update_review_flags_local_edits_without_hiding_the_diff) {
    const std::string installed = "# TDD\nmy own notes\n";
    const std::string remote = "# TDD\nremote text\n";
    LedgerEntry entry = entry_for("# TDD\noriginal\n");
    const UpdateReview review =
        build_update_review(entry, detail_for(remote, {"SKILL.md"}), installed, {});
    ASSERT_TRUE(review.local_modified);
    ASSERT_TRUE(review.needs_override());
    ASSERT_TRUE(review.has_changes());

    const auto lines = update_review_lines(review);
    ASSERT_TRUE(contains(lines, "local edits"));
    ASSERT_TRUE(contains(lines, "override"));
    ASSERT_TRUE(contains(lines, "my own notes")); // the user's line is visible in the diff
}

TEST(update_review_lists_added_and_removed_files_and_ignores_the_manifest) {
    const std::string body = "# TDD\n";
    const UpdateReview review = build_update_review(
        entry_for(body),
        detail_for(body, {"SKILL.md", "tests.md", "mocking.md", "nested/x.md"}, "aaa1111"), body,
        {"SKILL.md", "old.md"});
    ASSERT_FALSE(review.body_changed);
    ASSERT_TRUE(review.has_changes());
    ASSERT_EQ(review.added_files.size(), std::size_t{3});
    ASSERT_EQ(review.added_files[0], std::string("mocking.md"));
    ASSERT_EQ(review.removed_files.size(), std::size_t{1});
    ASSERT_EQ(review.removed_files[0], std::string("old.md"));

    const auto lines = update_review_lines(review);
    ASSERT_TRUE(contains(lines, "+3 files (mocking.md, nested/x.md, tests.md)"));
    ASSERT_TRUE(contains(lines, "-1 file (old.md)"));
}

TEST(update_review_reports_a_no_op_update) {
    const std::string body = "# TDD\nsame\n";
    const UpdateReview review = build_update_review(
        entry_for(body, "same"), detail_for(body, {"SKILL.md"}, "same"), body, {"SKILL.md"});
    ASSERT_FALSE(review.has_changes());
    const auto lines = update_review_lines(review);
    ASSERT_TRUE(contains(lines, "no changes"));
}

TEST(update_review_installed_files_are_relative_and_sorted) {
    const fs::path dir = fs::temp_directory_path() / "amber_update_review_files";
    fs::remove_all(dir);
    fs::create_directories(dir / "nested");
    for (const char* name : {"SKILL.md", "b.md", "nested/a.md"}) {
        std::ofstream(dir / name) << "x";
    }
    const auto files = installed_skill_files(dir.string());
    ASSERT_EQ(files.size(), std::size_t{3});
    ASSERT_EQ(files[0], std::string("SKILL.md"));
    ASSERT_EQ(files[1], std::string("b.md"));
    ASSERT_EQ(files[2], std::string("nested/a.md"));
    ASSERT_TRUE(installed_skill_files((dir / "missing").string()).empty());
    fs::remove_all(dir);
}
