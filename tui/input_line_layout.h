#ifndef AMBER_TUI_INPUT_LINE_LAYOUT_H
#define AMBER_TUI_INPUT_LINE_LAYOUT_H

#include <cstddef>
#include <string>
#include <vector>

// L1 (pure): the input line's prompt decor, horizontal scroll, and UTF-8
// column slicing. The painter (render_engine.cpp) maps roles to colour pairs.
namespace tui::input_line_layout {

// A prompt decor piece's role; the painter chooses the colour pair.
enum class Role { Decor, Project, Branch, Plus, Minus };

struct Piece {
    std::string text;
    Role role = Role::Decor;
};

// The prompt decor "└─[project]─[branch]─[+ins/-del]─❯ " as role-tagged pieces.
// The branch and diff parts are omitted when empty, as the shell did inline.
std::vector<Piece> prompt_pieces(const std::string& project, const std::string& branch, int ins,
                                 int del);

struct Scroll {
    int offset = 0;     // horizontal scroll offset, in columns
    int cursor_col = 0; // the cursor's column before scrolling
    int total_w = 0;    // prompt + input + shadow width
};

// Keep the cursor visible without scrolling the prompt away.
Scroll scroll_for(int prompt_w, const std::string& input, std::size_t cursor,
                  const std::string& shadow, int width);

// The byte length of `input[from..]` that fits in `room` display columns,
// never splitting a UTF-8 codepoint.
int visible_bytes(const std::string& input, std::size_t from, int room);

} // namespace tui::input_line_layout

#endif // AMBER_TUI_INPUT_LINE_LAYOUT_H
