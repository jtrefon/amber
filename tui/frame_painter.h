#ifndef AMBER_TUI_FRAME_PAINTER_H
#define AMBER_TUI_FRAME_PAINTER_H

// A small ncurses-shaped painter over a Surface. The shell's paint code was
// written against move/attron/mvaddnstr; this keeps that shape while the cells
// go into the frame surface instead of stdscr. Pair ids resolve to semantic
// roles, so the frame is styled by the theme like every migrated screen.

#include "tui/surface.h"

#include <string>

namespace tui {

namespace paint {

constexpr int color_pair(int pair) noexcept {
    return pair << 8;
}

enum : int {
    bold = 1 << 0,
    dim = 1 << 1,
    reverse = 1 << 2,
    underline = 1 << 3,
    italic = 1 << 4,
};

} // namespace paint

class FramePainter {
public:
    void begin(Surface& surface);

    void erase();
    void move(int y, int x);
    void attrset(int attrs);
    void attron(int attrs);
    void attroff(int attrs);

    void addch(char ch);
    void addnstr(const std::string& text, int max_columns);
    void addnwstr(const std::wstring& text, int max_columns);
    void mvaddnstr(int y, int x, const std::string& text, int max_columns);
    void mvaddnwstr(int y, int x, const std::wstring& text, int max_columns);
    void clrtoeol();
    void mvhline(int y, int x, char ch, int columns);

    // Copy a self-contained surface (the chat canvas, the drawer) into the frame.
    void blit(const Surface& surface, int x, int y);

    void show_cursor(bool visible) noexcept { cursor_visible_ = visible; }
    bool cursor_visible() const noexcept { return cursor_visible_; }
    int cursor_x() const noexcept { return x_; }
    int cursor_y() const noexcept { return y_; }

private:
    Style style() const;
    void put(const std::string& text, int max_columns);

    Surface* surface_ = nullptr;
    int x_ = 0;
    int y_ = 0;
    int attrs_ = 0;
    bool cursor_visible_ = false;
};

} // namespace tui

#endif // AMBER_TUI_FRAME_PAINTER_H
