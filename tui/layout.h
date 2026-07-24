// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_LAYOUT_H
#define AMBER_TUI_LAYOUT_H

namespace tui {

// Pure geometry — no rendering state.
struct Layout {
    int rows = 0, cols = 0;
    int chat_top = 0, chat_height = 0;
    int input_top = 0, input_height = 1;
    int status_top = 0;
    int palette_height = 0;

    void recalc(int term_rows, int term_cols) {
        rows = term_rows;
        cols = term_cols;
        chat_top = 0;
        status_top = rows - 1;
        input_top = status_top - 1;
        chat_height = input_top - chat_top;
    }
};

} // namespace tui

#endif // AMBER_TUI_LAYOUT_H
