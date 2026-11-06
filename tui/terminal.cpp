#include "tui/terminal.h"

#include "tui/keys.h"
#include "tui/textutil.h"
#include "tui/theme.h"

#include <ncurses.h>

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

    void register_pairs() {
        if (!has_colors())
            return;
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
    endwin();
    impl_->started = false;
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

} // namespace tui
