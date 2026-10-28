#include "agent/text_diff.h"

#include <algorithm>
#include <cstdint>

namespace agent {

namespace {

// 2000 x 2000 differing lines is 16 MB of table and a visible stall; beyond
// that the answer would not be more precise in any way the reader cares about.
constexpr std::size_t kMaxDiffCells = 4'000'000;

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::size_t cell(const std::vector<std::uint32_t>& table, std::size_t width, std::size_t row,
                 std::size_t col) noexcept {
    return table[row * width + col];
}

// Longest common subsequence lengths, bottom-up, in one flat table.
std::vector<std::uint32_t> lcs_table(const std::vector<std::string>& a,
                                     const std::vector<std::string>& b) {
    const std::size_t width = b.size() + 1;
    std::vector<std::uint32_t> table((a.size() + 1) * width, 0);
    for (std::size_t i = a.size(); i-- > 0;) {
        for (std::size_t j = b.size(); j-- > 0;) {
            table[i * width + j] =
                a[i] == b[j] ? cell(table, width, i + 1, j + 1) + 1
                             : std::max(cell(table, width, i + 1, j), cell(table, width, i, j + 1));
        }
    }
    return table;
}

std::vector<DiffLine> coarse(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<DiffLine> out;
    out.reserve(a.size() + b.size());
    for (const auto& line : a)
        out.push_back({DiffLine::Kind::Removed, line});
    for (const auto& line : b)
        out.push_back({DiffLine::Kind::Added, line});
    return out;
}

char marker(DiffLine::Kind kind) noexcept {
    switch (kind) {
    case DiffLine::Kind::Added:
        return '+';
    case DiffLine::Kind::Removed:
        return '-';
    case DiffLine::Kind::Context:
        break;
    }
    return ' ';
}

struct Hunk {
    std::size_t first = 0; // inclusive index into the diff lines
    std::size_t last = 0;  // exclusive
};

std::vector<Hunk> hunks_of(const std::vector<DiffLine>& lines, int context) {
    const std::size_t around = static_cast<std::size_t>(std::max(0, context));
    std::vector<Hunk> hunks;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLine::Kind::Context)
            continue;
        const std::size_t first = i > around ? i - around : 0;
        const std::size_t last = std::min(lines.size(), i + around + 1);
        if (!hunks.empty() && first <= hunks.back().last)
            hunks.back().last = last;
        else
            hunks.push_back({first, last});
    }
    return hunks;
}

} // namespace

std::vector<DiffLine> diff_lines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = split_lines(before);
    const std::vector<std::string> b = split_lines(after);
    if (a == b) {
        std::vector<DiffLine> same;
        same.reserve(a.size());
        for (const auto& line : a)
            same.push_back({DiffLine::Kind::Context, line});
        return same;
    }
    if (a.size() * b.size() > kMaxDiffCells)
        return coarse(a, b);

    const std::size_t width = b.size() + 1;
    const std::vector<std::uint32_t> table = lcs_table(a, b);

    std::vector<DiffLine> out;
    out.reserve(a.size() + b.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back({DiffLine::Kind::Context, a[i]});
            ++i;
            ++j;
        } else if (cell(table, width, i + 1, j) >= cell(table, width, i, j + 1)) {
            out.push_back({DiffLine::Kind::Removed, a[i]});
            ++i;
        } else {
            out.push_back({DiffLine::Kind::Added, b[j]});
            ++j;
        }
    }
    for (; i < a.size(); ++i)
        out.push_back({DiffLine::Kind::Removed, a[i]});
    for (; j < b.size(); ++j)
        out.push_back({DiffLine::Kind::Added, b[j]});
    return out;
}

namespace {

std::string hunk_header(std::size_t a_start, std::size_t a_count, std::size_t b_start,
                        std::size_t b_count) {
    return "@@ -" + std::to_string(a_start + (a_count > 0 ? 1 : 0)) + "," +
           std::to_string(a_count) + " +" + std::to_string(b_start + (b_count > 0 ? 1 : 0)) + "," +
           std::to_string(b_count) + " @@";
}

struct Range {
    std::size_t a_count = 0;
    std::size_t b_count = 0;
};

Range range_of(const std::vector<DiffLine>& lines, const Hunk& hunk) {
    Range range;
    for (std::size_t i = hunk.first; i < hunk.last; ++i) {
        if (lines[i].kind != DiffLine::Kind::Added)
            ++range.a_count;
        if (lines[i].kind != DiffLine::Kind::Removed)
            ++range.b_count;
    }
    return range;
}

// Advances the file positions over the lines a hunk skipped.
void advance_positions(const std::vector<DiffLine>& lines, std::size_t first, std::size_t last,
                       std::size_t& a_pos, std::size_t& b_pos) {
    for (std::size_t i = first; i < last; ++i) {
        if (lines[i].kind != DiffLine::Kind::Added)
            ++a_pos;
        if (lines[i].kind != DiffLine::Kind::Removed)
            ++b_pos;
    }
}

} // namespace

std::vector<std::string> render_unified_diff(const std::string& path, const std::string& before,
                                             const std::string& after, int context,
                                             std::size_t max_lines) {
    const std::vector<DiffLine> lines = diff_lines(before, after);
    const std::vector<Hunk> hunks = hunks_of(lines, context);
    if (hunks.empty())
        return {path + ": no changes"};

    std::vector<std::string> out;
    out.reserve(max_lines + 1);
    std::size_t a_pos = 0;
    std::size_t b_pos = 0;
    std::size_t cursor = 0;
    for (const Hunk& hunk : hunks) {
        advance_positions(lines, cursor, hunk.first, a_pos, b_pos);
        const Range range = range_of(lines, hunk);
        out.push_back(hunk_header(a_pos, range.a_count, b_pos, range.b_count));
        for (std::size_t i = hunk.first; i < hunk.last; ++i) {
            if (out.size() >= max_lines) {
                out.push_back("... " + std::to_string(lines.size() - i) + " more lines");
                return out;
            }
            out.push_back(std::string(1, marker(lines[i].kind)) + lines[i].text);
        }
        a_pos += range.a_count;
        b_pos += range.b_count;
        cursor = hunk.last;
    }
    return out;
}

DiffCounts count_diff(const std::vector<DiffLine>& lines) noexcept {
    DiffCounts counts;
    for (const auto& line : lines) {
        if (line.kind == DiffLine::Kind::Added)
            ++counts.added;
        else if (line.kind == DiffLine::Kind::Removed)
            ++counts.removed;
    }
    return counts;
}

} // namespace agent
