#include "tui/input_line_layout.h"

#include <algorithm>

#include "tui/textutil.h"

namespace tui::input_line_layout {

namespace {
// └─[   ]─[   ]─❯
constexpr const char* kDecorOpen = "\u2514\u2500[";
constexpr const char* kDecorClose = "]\u2500[";
constexpr const char* kDecorEnd = "]\u2500\u276f ";
} // namespace

std::vector<Piece> prompt_pieces(const std::string& project, const std::string& branch, int ins,
                                 int del) {
    std::vector<Piece> out;
    out.push_back({kDecorOpen, Role::Decor});
    out.push_back({project, Role::Project});
    if (!branch.empty()) {
        out.push_back({kDecorClose, Role::Decor});
        out.push_back({branch, Role::Branch});
        if (ins > 0 || del > 0) {
            out.push_back({kDecorClose, Role::Decor});
            if (ins > 0)
                out.push_back({"+" + std::to_string(ins), Role::Plus});
            out.push_back({"/", Role::Decor});
            if (del > 0)
                out.push_back({"-" + std::to_string(del), Role::Minus});
        }
    }
    out.push_back({kDecorEnd, Role::Decor});
    return out;
}

Scroll scroll_for(int prompt_w, const std::string& input, std::size_t cursor,
                  const std::string& shadow, int width) {
    Scroll s;
    s.total_w = prompt_w + text::display_cols(input) + text::display_cols(shadow);
    s.cursor_col = prompt_w + text::display_cols(input.substr(0, cursor));
    if (s.cursor_col >= width)
        s.offset = s.cursor_col - width + 1;
    if (s.total_w - s.offset <= 0)
        s.offset = std::max(0, s.total_w - width);
    return s;
}

int visible_bytes(const std::string& input, std::size_t from, int room) {
    int visible_cols = 0;
    std::size_t end = from;
    while (end < input.size() && visible_cols < room) {
        const std::size_t adv = text::utf8_len(input, end);
        const std::string cp = input.substr(end, adv);
        const int cw = text::display_cols(cp);
        if (visible_cols + cw > room)
            break;
        visible_cols += cw;
        end += adv;
    }
    return static_cast<int>(end - from);
}

Block block_for(const std::string& input, std::size_t cursor, int max_rows) {
    Block b;
    const int rows = std::max(1, max_rows);
    const std::size_t clamped = std::min(cursor, input.size());

    std::size_t start = 0;
    std::size_t seen = 0;
    for (;;) {
        const std::size_t nl = input.find('\n', start);
        const std::size_t end = (nl == std::string::npos) ? input.size() : nl;
        if (clamped >= start && clamped <= end) {
            b.cursor_row = static_cast<int>(seen);
            b.cursor_col = static_cast<int>(clamped - start);
        }
        b.lines.push_back(input.substr(start, end - start));
        ++seen;
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }

    const int total = static_cast<int>(b.lines.size());
    b.rows = std::min(rows, total);
    b.first = std::max(0, b.cursor_row - b.rows + 1);
    if (b.cursor_row < b.first)
        b.cursor_row = b.first;
    return b;
}

} // namespace tui::input_line_layout
