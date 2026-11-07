#include "tui/terminal.h"

#include "tui/keys.h"
#include "tui/widgets.h"
#include "tui/textutil.h"
#include "tui/theme.h"

#include <ncurses.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace tui {
// The ncurses codes the adapter reads are the codes the L1 vocabulary names.
// If ncurses changes its ABI these fail the build, and the adapter must
// translate instead of passing raw codes through.
static_assert(tui::keys::kUp == KEY_UP, "ncurses KEY_UP drifted from tui::keys");
static_assert(tui::keys::kDown == KEY_DOWN, "ncurses KEY_DOWN drifted from tui::keys");
static_assert(tui::keys::kLeft == KEY_LEFT, "ncurses KEY_LEFT drifted from tui::keys");
static_assert(tui::keys::kRight == KEY_RIGHT, "ncurses KEY_RIGHT drifted from tui::keys");
static_assert(tui::keys::kHome == KEY_HOME, "ncurses KEY_HOME drifted from tui::keys");
static_assert(tui::keys::kEnd == KEY_END, "ncurses KEY_END drifted from tui::keys");
static_assert(tui::keys::kBackspace == KEY_BACKSPACE,
              "ncurses KEY_BACKSPACE drifted from tui::keys");
static_assert(tui::keys::kDelete == KEY_DC, "ncurses KEY_DC drifted from tui::keys");
static_assert(tui::keys::kNPage == KEY_NPAGE, "ncurses KEY_NPAGE drifted from tui::keys");
static_assert(tui::keys::kPPage == KEY_PPAGE, "ncurses KEY_PPAGE drifted from tui::keys");
static_assert(tui::keys::kEnter == KEY_ENTER, "ncurses KEY_ENTER drifted from tui::keys");
static_assert(tui::keys::kBtab == KEY_BTAB, "ncurses KEY_BTAB drifted from tui::keys");
static_assert(tui::keys::kF10 == KEY_F(10), "ncurses KEY_F(10) drifted from tui::keys");
static_assert(tui::keys::kMouse == KEY_MOUSE, "ncurses KEY_MOUSE drifted from tui::keys");
static_assert(tui::keys::kResize == KEY_RESIZE, "ncurses KEY_RESIZE drifted from tui::keys");
static_assert(tui::keys::kButton1 == BUTTON1_PRESSED,
              "ncurses BUTTON1_PRESSED drifted from tui::keys");
static_assert(tui::keys::kButton4 == BUTTON4_PRESSED,
              "ncurses BUTTON4_PRESSED drifted from tui::keys");
#ifdef BUTTON5_PRESSED
static_assert(tui::keys::kButton5 == BUTTON5_PRESSED,
              "ncurses BUTTON5_PRESSED drifted from tui::keys");
#endif

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

int pair_for(const Style& style) {
    return style.exact_pair > 0 ? style.exact_pair : kPairBase + static_cast<int>(style.role);
}

int color_for(const Style& style, bool colors) {
    const int pair = pair_for(style);
    if (colors && has_colors() && pair < COLOR_PAIRS)
        return COLOR_PAIR(pair);
    return 0;
}

int emphasis_for(const Style& style, const RoleStyle& role) {
    int attrs = 0;
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

int attr_for(const Style& style, const Theme& theme, bool colors) {
    // The role carries the shipped emphasis (a title is bold, muted text is
    // dim); the cell may only add to it, never cancel it.
    return color_for(style, colors) | emphasis_for(style, theme.style(style.role));
}

} // namespace

struct Terminal::Impl {
    Theme theme = Theme::amber(GlyphMode::Unicode);
    bool started = false;
    bool pairs_registered = false;
    bool colors = true;
    WINDOW* overlay = nullptr;
    int overlay_w = 0;
    int overlay_h = 0;

    void register_pairs() {
        if (!colors || !has_colors() || pairs_registered)
            return;
        pairs_registered = true;
        for (int i = 0; i < static_cast<int>(Role::Count); ++i) {
            const RoleStyle& role = theme.style(static_cast<Role>(i));
            const int pair = kPairBase + i;
            if (pair >= COLOR_PAIRS)
                break;
            init_pair(static_cast<short>(pair), static_cast<short>(color_id(role.fg)),
                      static_cast<short>(color_id(role.bg)));
        }
        // The welcome raster is xterm's 232..255 grayscale ramp, registered as
        // fixed pairs rather than redefining palette entries (OSC 4 output).
        init_pair(P_GRAY, 0, 0);
        for (int i = 0; i < 24; ++i) {
            const int pair = P_GRAY + 1 + i;
            if (pair >= COLOR_PAIRS)
                break;
            if (COLORS >= 256)
                init_pair(static_cast<short>(pair), static_cast<short>(232 + i),
                          static_cast<short>(232 + i));
            else
                init_pair(static_cast<short>(pair), COLOR_WHITE, COLOR_BLACK);
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
    raw(); // Ctrl-C is a keypress, not SIGINT
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE); // the high-bit Alt+digit form
    set_escdelay(25);
    curs_set(1);
    const char* no_color = std::getenv("NO_COLOR");
    impl_->colors = !(no_color && *no_color);
    if (impl_->colors) {
        start_color();
        use_default_colors();
    }
    use_legacy_coding(1);
    impl_->register_pairs();
    set_mouse(true);
    mouseinterval(0);
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

void Terminal::set_theme(const Theme& theme) {
    impl_->theme = theme;
    impl_->pairs_registered = false;
    impl_->register_pairs();
}

void Terminal::place_cursor(int x, int y) {
    move(y, x);
    curs_set(1);
}

void Terminal::hide_cursor() {
    curs_set(0);
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
        attrset(attr_for(damage.cell.style, impl_->theme, impl_->colors));
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

int Terminal::read_key_timeout(int milliseconds) {
    timeout(milliseconds);
    const int key = getch();
    timeout(kTickTimeoutMs);
    return key == ERR ? keys::kNone : key;
}

void Terminal::push_key(int key) {
    ungetch(key);
}

bool Terminal::read_mouse(keys::MouseEvent& event) {
    MEVENT raw{};
    if (getmouse(&raw) != OK)
        return false;
    event = keys::MouseEvent{raw.x, raw.y, static_cast<keys::MouseMask>(raw.bstate)};
    return true;
}

void Terminal::set_mouse(bool enabled) {
    mousemask(enabled ? (BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED) : 0, nullptr);
}

void Terminal::flush() {
    wnoutrefresh(stdscr);
    doupdate();
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
            wattrset(win, attr_for(cell.style, impl_->theme, impl_->colors));
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

void Terminal::blit(const Surface& surface, int x, int y) {
    impl_->register_pairs();
    const Size screen = size();
    for (int row = 0; row < surface.height(); ++row) {
        const int target_y = y + row;
        if (target_y < 0 || target_y >= screen.h)
            continue;
        for (int column = 0; column < surface.width(); ++column) {
            const int target_x = x + column;
            if (target_x < 0 || target_x >= screen.w)
                continue;
            const Cell& cell = surface.at(column, row);
            if (cell.continuation)
                continue;
            attrset(attr_for(cell.style, impl_->theme, impl_->colors));
            const std::wstring glyph = text::to_wide(cell.glyph);
            if (!glyph.empty())
                mvaddnwstr(target_y, target_x, glyph.c_str(), static_cast<int>(glyph.size()));
        }
    }
    // The shell paints after the drawer with attron(), which adds to whatever
    // is current; leaving the last cell's pair/bold on would colour the input
    // line that repaints immediately below it.
    attrset(A_NORMAL);
}

} // namespace tui
