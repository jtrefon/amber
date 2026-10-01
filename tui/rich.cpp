
#include "rich.h"

#include <algorithm>

#include "textutil.h"

namespace tui::rich {

int cols(const std::string& s) {
    return text::display_cols(s);
}

namespace {

// Split `text` into words and residual whitespace, keeping each token's byte
// range so we can slice the original (preserving UTF-8) rather than rebuild.
// A word is a maximal run of non-space; spaces between words are kept as
// separate tokens so wrapping preserves a single leading space.
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t')
            j += text::utf8_len(s, j);
        if (j > i) {
            out.push_back(s.substr(i, j - i));
            i = j;
        } else { // whitespace run
            size_t k = i;
            while (k < n && (s[k] == ' ' || s[k] == '\t'))
                ++k;
            out.push_back(s.substr(i, k - i));
            i = k;
        }
    }
    return out;
}

// One token plus the run it came from, so a wrapped line keeps its styles.
struct Piece {
    std::string text;
    size_t run = 0;
};

// A piece carries inter-word whitespace when `tokens` split it out as its own
// token; it wraps as a single column and is dropped at a line start.
bool is_space_piece(const std::string& t) {
    return t.find(' ') != std::string::npos || t.find('\t') != std::string::npos;
}

// Flatten the styled runs into (token, run-index) pieces. A single token wider
// than the column is hard-broken so it cannot overflow the canvas - only safe
// on pure ASCII, where a byte boundary is a display column; multi-byte tokens
// are left intact (the terminal clips them rather than corrupting them).
std::vector<Piece> wrap_pieces(const Line& in, int width) {
    std::vector<Piece> pieces;
    pieces.reserve(in.runs.size() * 2);
    for (size_t ri = 0; ri < in.runs.size(); ++ri)
        for (const auto& t : tokens(in.runs[ri].text)) {
            if (t.empty())
                continue;
            const bool ascii =
                std::all_of(t.begin(), t.end(), [](char c) { return (unsigned char)c < 0x80; });
            if (ascii && cols(t) > width) {
                const auto step = static_cast<size_t>(width);
                for (size_t o = 0; o < t.size(); o += step)
                    pieces.push_back({t.substr(o, step), ri});
            } else {
                pieces.push_back({t, ri});
            }
        }
    return pieces;
}

// Merge the just-pushed run into the previous one when the style matches, so a
// wrapped physical line keeps a compact run list.
void merge_tail(std::vector<Run>& runs) {
    const size_t n = runs.size();
    if (n < 2)
        return;
    Run& a = runs[n - 2];
    const Run& b = runs[n - 1];
    if (a.pair == b.pair && a.bold == b.bold && a.dim == b.dim && a.italic == b.italic &&
        a.under == b.under) {
        a.text += b.text;
        runs.pop_back();
    }
}

// Push the accumulated line, carrying the block flags from the source line.
void flush_line(std::vector<Line>& out, Line& cur, const Line& in, int& used, bool& first) {
    cur.is_code = in.is_code;
    cur.is_hr = in.is_hr;
    cur.is_table = in.is_table;
    cur.heading = in.heading;
    out.push_back(std::move(cur));
    cur = Line{};
    used = 0;
    first = true;
}

} // namespace

std::vector<Line> wrap(const Line& in, int width) {
    if (width <= 0)
        width = 80;
    const std::vector<Piece> pieces = wrap_pieces(in, width);

    std::vector<Line> out;
    Line cur;
    int used = 0;
    bool first = true;
    for (const auto& p : pieces) {
        const int w = cols(p.text);
        const bool space = is_space_piece(p.text);
        if (!first && used + (space ? 1 : w) > width) {
            flush_line(out, cur, in, used, first);
            if (space)
                continue; // drop the leading space at line start
        }
        Run r = in.runs[p.run];
        r.text = (!first && space) ? " " : p.text;
        cur.runs.push_back(r);
        merge_tail(cur.runs);
        used += (first && space) ? 0 : w;
        first = false;
    }
    if (!cur.runs.empty() || out.empty())
        flush_line(out, cur, in, used, first);
    return out;
}

std::vector<Line> rewrap_all(const std::vector<Line>& lines, int width) {
    std::vector<Line> out;
    if (width <= 0)
        width = 80;
    for (const auto& l : lines) {
        if (l.is_hr || l.is_table) {
            out.push_back(l);
            continue;
        }
        auto wl = wrap(l, width);
        for (auto& x : wl)
            out.push_back(std::move(x));
    }
    return out;
}

} // namespace tui::rich
