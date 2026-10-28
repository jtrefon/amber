// Line diff for the skill update review: accuracy of context/add/remove, the
// hunk grouping, the output cap, and the bounded fallback for huge inputs.

#include "agent/text_diff.h"

#include "test_util.h"

#include <string>
#include <vector>

using namespace agent;

namespace {

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines)
        out += line + "\n";
    return out;
}

const std::vector<std::string> kBefore = {"# TDD", "", "Write the test first.", "Run it.",
                                          "Then implement."};
const std::vector<std::string> kAfter = {
    "# TDD", "", "Write the test first.", "Watch it fail.", "Then implement.", "", "## Seams"};

} // namespace

TEST(text_diff_marks_added_removed_and_context_lines) {
    const auto lines = diff_lines(join(kBefore), join(kAfter));
    int added = 0;
    int removed = 0;
    int context = 0;
    for (const auto& line : lines) {
        if (line.kind == DiffLine::Kind::Added)
            ++added;
        else if (line.kind == DiffLine::Kind::Removed)
            ++removed;
        else
            ++context;
    }
    ASSERT_EQ(added, 3);   // "Watch it fail.", "", "## Seams"
    ASSERT_EQ(removed, 1); // "Run it."
    ASSERT_EQ(context, 4); // the shared lines

    const DiffCounts counts = count_diff(lines);
    ASSERT_EQ(counts.added, 3);
    ASSERT_EQ(counts.removed, 1);
}

TEST(text_diff_identical_text_is_all_context) {
    const auto lines = diff_lines("same\ntext\n", "same\ntext\n");
    ASSERT_EQ(lines.size(), std::size_t{2});
    for (const auto& line : lines)
        ASSERT_TRUE(line.kind == DiffLine::Kind::Context);
    const auto rendered = render_unified_diff("SKILL.md", "same\n", "same\n");
    ASSERT_EQ(rendered.size(), std::size_t{1});
    ASSERT_EQ(rendered[0], std::string("SKILL.md: no changes"));
}

TEST(text_diff_renders_hunks_with_a_header) {
    const auto rendered = render_unified_diff("SKILL.md", join(kBefore), join(kAfter));
    ASSERT(rendered.size() > 1);
    ASSERT_EQ(rendered[0].rfind("@@ -", 0), std::size_t{0});
    bool saw_removed = false;
    bool saw_added = false;
    for (const auto& line : rendered) {
        if (line.rfind("-Run it.", 0) == 0)
            saw_removed = true;
        if (line.rfind("+Watch it fail.", 0) == 0)
            saw_added = true;
    }
    ASSERT_TRUE(saw_removed);
    ASSERT_TRUE(saw_added);
}

TEST(text_diff_context_is_trimmed_to_the_configured_radius) {
    std::string before;
    std::string after;
    for (int i = 0; i < 40; ++i) {
        const std::string line = "line " + std::to_string(i);
        before += line + "\n";
        after += (i == 20 ? "changed" : line) + "\n";
    }
    const std::string narrow = "SKILL.md";
    const auto tight = render_unified_diff(narrow, before, after, 1);
    // header + one context line either side + the removed and the added line.
    ASSERT_EQ(tight.size(), std::size_t{5});

    const auto wide = render_unified_diff(narrow, before, after, 5);
    ASSERT_EQ(wide.size(), std::size_t{13});
}

TEST(text_diff_output_is_capped_with_a_remainder_marker) {
    std::string before;
    std::string after;
    for (int i = 0; i < 200; ++i) {
        before += "old " + std::to_string(i) + "\n";
        after += "new " + std::to_string(i) + "\n";
    }
    const auto rendered = render_unified_diff("SKILL.md", before, after, 3, 20);
    ASSERT_EQ(rendered.size(), std::size_t{21});
    ASSERT_TRUE(rendered.back().find("more lines") != std::string::npos);
}

TEST(text_diff_huge_inputs_fall_back_to_replace) {
    std::string before;
    std::string after;
    for (int i = 0; i < 2100; ++i) {
        before += "a" + std::to_string(i) + "\n";
        after += "b" + std::to_string(i) + "\n";
    }
    const auto lines = diff_lines(before, after);
    ASSERT_EQ(lines.size(), std::size_t{4200});
    ASSERT_TRUE(lines.front().kind == DiffLine::Kind::Removed);
    ASSERT_TRUE(lines.back().kind == DiffLine::Kind::Added);
}

TEST(text_diff_handles_empty_sides) {
    const auto added = diff_lines("", "one\ntwo\n");
    ASSERT_EQ(added.size(), std::size_t{2});
    ASSERT_TRUE(added[0].kind == DiffLine::Kind::Added);

    const auto removed = diff_lines("one\ntwo\n", "");
    ASSERT_EQ(removed.size(), std::size_t{2});
    ASSERT_TRUE(removed[0].kind == DiffLine::Kind::Removed);

    ASSERT_TRUE(diff_lines("", "").empty());
}
