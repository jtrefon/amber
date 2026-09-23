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
    if (s.offset < prompt_w)
        s.offset = 0;
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

} // namespace tui::input_line_layout
