#include "tui/terminal.h"

#include "tui/keys.h"
#include "tui/textutil.h"
#include "tui/theme.h"

#include <ncurses.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace tui {

namespace {

// Theme pairs live above the shell's hand-numbered ids (pairs.h starts at 1 and
// the grayscale art block starts at 100) so the adapter can own its table
// without renumbering every existing pair.
constexpr int kPairBase = 200;

int color_id(Color color) {
    if (!has_colors())
        return -1;
    switch (color) {
    case Color::Default:
        return -1;
    case Color::Black:
        return COLOR_BLACK;
    case Color::Red:
        return COLOR_RED;
    case Color::Green:
        return COLOR_GREEN;
    case Color::Yellow:
        return COLOR_YELLOW;
    case Color::Blue:
        return COLOR_BLUE;
    case Color::Magenta:
        return COLOR_MAGENTA;
    case Color::Cyan:
        return COLOR_CYAN;
    case Color::White:
        return COLOR_WHITE;
    case Color::DarkGray:
        return COLORS > 8 ? 8 : COLOR_WHITE; // 8-colour terminals have no grey
    }
    return -1;
}

int attr_for(const Style& style, const Theme& theme) {
    int attrs = 0;
    const int pair = kPairBase + static_cast<int>(style.role);
    if (has_colors() && pair < COLOR_PAIRS)
        attrs |= COLOR_PAIR(pair);
    // The role carries the shipped emphasis (a title is bold, muted text is
    // dim); the cell may only add to it, never cancel it.
    const RoleStyle& role = theme.style(style.role);
    if (style.bold || role.bold)
        attrs |= A_BOLD;
    if (style.dim || role.dim)
        attrs |= A_DIM;
    if (style.italic || role.italic)
        attrs |= A_ITALIC;
    if (style.underline || role.underline)
        attrs |= A_UNDERLINE;
    if (style.reverse || role.reverse)
        attrs |= A_REVERSE;
    return attrs;
}

} // namespace

struct Terminal::Impl {
    Theme theme = Theme::amber(GlyphMode::Unicode);
    bool started = false;
    bool pairs_registered = false;
    WINDOW* overlay = nullptr;
    int overlay_w = 0;
    int overlay_h = 0;

    void register_pairs() {
        if (!has_colors() || pairs_registered)
            return;
        pairs_registered = true;
        start_color();
        use_default_colors();
        for (int i = 0; i < static_cast<int>(Role::Count); ++i) {
            const RoleStyle& role = theme.style(static_cast<Role>(i));
            const int pair = kPairBase + i;
            if (pair >= COLOR_PAIRS)
                break;
            init_pair(static_cast<short>(pair), static_cast<short>(color_id(role.fg)),
                      static_cast<short>(color_id(role.bg)));
        }
    }
};

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {}

Terminal::~Terminal() {
    stop();
}

void Terminal::start() {
    if (impl_->started)
        return;
    if (!initscr())
        throw std::runtime_error("initscr failed");
    impl_->started = true;
    raw();
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE);
    curs_set(0);
    impl_->register_pairs();
}

void Terminal::stop() {
    if (!impl_ || !impl_->started)
        return;
    close_overlay();
    endwin();
    impl_->started = false;
}

void Terminal::adopt() {
    impl_->register_pairs();
}

void Terminal::set_timeout(int milliseconds) {
    timeout(milliseconds);
}

int Terminal::read_key_now() {
    timeout(0);
    const int key = getch();
    timeout(-1);
    return key == ERR ? keys::kNone : key;
}

Size Terminal::size() const {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    return {columns, rows};
}

void Terminal::present(Surface& surface) {
    for (const Damage& damage : surface.diff()) {
        attrset(attr_for(damage.cell.style, impl_->theme));
        const std::wstring glyph = text::to_wide(damage.cell.glyph);
        if (!glyph.empty())
            mvaddnwstr(damage.y, damage.x, glyph.c_str(), static_cast<int>(glyph.size()));
    }
    wnoutrefresh(stdscr);
    doupdate();
    surface.commit();
}

void Terminal::clear_screen() {
    erase();
    clearok(stdscr, TRUE);
}

int Terminal::read_key() {
    const int key = getch();
    return key == ERR ? keys::kNone : key;
}

void Terminal::overlay(const Surface& surface, int x, int y, int cursor_x, int cursor_y) {
    const Size screen = size();
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int width = std::min(surface.width() + x - left, screen.w - left);
    const int height = std::min(surface.height() + y - top, screen.h - top);
    if (width <= 0 || height <= 0)
        return;
    if (!impl_->overlay || impl_->overlay_w != width || impl_->overlay_h != height) {
        if (impl_->overlay)
            delwin(impl_->overlay);
        impl_->overlay = newwin(height, width, top, left);
        impl_->overlay_w = width;
        impl_->overlay_h = height;
    }
    WINDOW* win = impl_->overlay;
    werase(win);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const Cell& cell = surface.at(column + left - x, row + top - y);
            if (cell.continuation)
                continue;
            wattrset(win, attr_for(cell.style, impl_->theme));
            const std::wstring glyph = text::to_wide(cell.glyph);
            if (!glyph.empty())
                mvwaddnwstr(win, row, column, glyph.c_str(), static_cast<int>(glyph.size()));
        }
    }
    if (cursor_x >= 0 && cursor_y >= 0) {
        wmove(win, cursor_y, cursor_x);
        curs_set(1);
    } else {
        curs_set(0);
    }
    wnoutrefresh(win);
    doupdate();
}

void Terminal::close_overlay() {
    if (!impl_ || !impl_->overlay)
        return;
    delwin(impl_->overlay);
    impl_->overlay = nullptr;
    impl_->overlay_w = 0;
    impl_->overlay_h = 0;
    curs_set(0);
    // The overlay wrote into the virtual screen; stdscr still holds the frame
    // the dialog covered, so one full refresh restores it.
    touchwin(stdscr);
    wnoutrefresh(stdscr);
    doupdate();
}

} // namespace tui
