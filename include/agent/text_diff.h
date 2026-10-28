#ifndef AMBER_AGENT_TEXT_DIFF_H
#define AMBER_AGENT_TEXT_DIFF_H

#include <cstddef>
#include <string>
#include <vector>

namespace agent {

// One line of a line-level diff. `text` never carries its newline.
struct DiffLine {
    enum class Kind : unsigned char { Context, Added, Removed };

    Kind kind = Kind::Context;
    std::string text;
};

// Line diff of two texts (longest common subsequence). The table is bounded:
// past `kMaxDiffCells` lines are reported as removed-then-added, because a
// quadratic table on two large bodies would stall the UI for an answer no more
// useful than "all of this is new".
std::vector<DiffLine> diff_lines(const std::string& before, const std::string& after);

// Unified-style rendering for the review pager: `context` unchanged lines
// around each change, hunk headers, and at most `max_lines` lines with a
// trailing marker when the diff is longer. This is a readable review, not a
// patch amber applies, so the header only has to be honest about its ranges.
std::vector<std::string> render_unified_diff(const std::string& path, const std::string& before,
                                             const std::string& after, int context = 3,
                                             std::size_t max_lines = 400);

// How many lines a diff added and removed, for the one-line summary the
// explorer and the CLI print.
struct DiffCounts {
    int added = 0;
    int removed = 0;
};

DiffCounts count_diff(const std::vector<DiffLine>& lines) noexcept;

} // namespace agent

#endif // AMBER_AGENT_TEXT_DIFF_H
