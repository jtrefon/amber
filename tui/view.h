// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#pragma once

#include <string>
#include <vector>

namespace tui {

// Pure interface that the Tui::run() event loop uses for all user-facing IO.
// Production: NcursesView delegates to ncurses.
// Test: MockView records events for assertions.
class View {
public:
    virtual ~View() = default;

    // ── Rendering ───────────────────────────────────────────────────

    virtual void draw() = 0;
    virtual void draw_input(const std::string& input, size_t cursor,
                            const std::string& shadow) = 0;
    virtual void draw_status_bar(const std::string& tail) = 0;
    virtual void flush() = 0;
    virtual void clear_screen() = 0;

    // ── Input ───────────────────────────────────────────────────────

    enum Key : int {
        KEY_NONE  = -1,
        KEY_ERR   = -2,
        KEY_UP    = -3,
        KEY_DOWN  = -4,
        KEY_LEFT  = -5,
        KEY_RIGHT = -6,
        KEY_HOME  = -7,
        KEY_END   = -8,
        KEY_PPAGE = -9,
        KEY_NPAGE = -10,
        KEY_BACKSPACE = -11,
        KEY_TAB   = -12,
        KEY_SHIFT_TAB = -13,
        KEY_ENTER = -14,
        KEY_ESC   = -15,

        // Printable range: 32-126 map to ASCII.
        // Ctrl range: 1-26 map to Ctrl-A through Ctrl-Z.
    };

    // Get the next key event. Blocks until one is available.
    virtual int get_key() = 0;

    // Set the timeout for get_key() (milliseconds).
    virtual void set_timeout(int ms) = 0;

    // ── Dialogs ─────────────────────────────────────────────────────

    // Show a selection menu. Returns selected index or -1 for cancel.
    virtual int menu_select(const std::string& title,
                            const std::vector<std::string>& items) = 0;

    // ── Misc ────────────────────────────────────────────────────────

    virtual int terminal_height() const = 0;
    virtual int terminal_width() const = 0;

    // Append a line to the scrollback (for status/error messages).
    virtual void append_line(int color, const std::string& text) = 0;
};

} // namespace tui
