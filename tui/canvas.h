// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_CANVAS_H
#define AMBER_TUI_CANVAS_H

#include <string>
#include <vector>

#include "rich.h"

namespace tui {

// A dedicated ncurses WINDOW for the chat scrollback. Unlike painting onto
// stdscr, a real WINDOW lets the chat own its color/attribute context, scroll
// independently of the status bar/input line, and host rich (multi-run) lines.
//
// Lines are stored as RichLines; the canvas wraps each to the current width and
// keeps a viewport offset (`top`) for software scrolling. Rendering uses
// mvwaddnwstr so multi-byte glyphs land in single cells correctly.
class Canvas {
public:
    Canvas();
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // Resize the underlying window to (h rows x w cols) at screen row `y`.
    void resize(int y, int h, int w);

    void set_lines(const std::vector<rich::Line>& lines);
    void clear_lines();

    // Viewport (first wrapped row shown).
    int top() const { return top_; }
    void set_top(int t) { top_ = t; }
    int wrapped_count() const { return static_cast<int>(wrapped_.size()); }
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int max_top() const { return std::max(0, wrapped_count() - rows_); }

    // Paint the current viewport into the window and refresh it.
    void render();

private:
    void rewrap();

    WINDOW* win_ = nullptr;
    int y_ = 0, rows_ = 0, cols_ = 0;
    int top_ = 0;
    std::vector<rich::Line> lines_;
    std::vector<rich::Line> wrapped_;
};

} // namespace tui

#endif // AMBER_TUI_CANVAS_H
