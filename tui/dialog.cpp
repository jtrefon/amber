// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "widgets.h"

#include <panel.h>

namespace tui {

void init_pairs() {
    init_pair(P_USER,       COLOR_GREEN,  -1);
    init_pair(P_ASSISTANT,  COLOR_CYAN,   -1);
    init_pair(P_STATUS,     COLOR_YELLOW, -1);
    init_pair(P_REASONING,  COLOR_WHITE,  -1);
    init_pair(P_BANNER,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_FIELD,      COLOR_WHITE,  COLOR_BLACK);
    init_pair(P_FIELD_ACT,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(P_DIALOG,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_BUTTON,     COLOR_BLACK,  COLOR_WHITE);
    init_pair(P_BUTTON_ACT, COLOR_WHITE,  COLOR_MAGENTA);
    init_pair(P_SHADOW,     COLOR_BLACK,  COLOR_BLACK);
    init_pair(P_GAUGE_OK,   COLOR_GREEN,   COLOR_BLUE);
    init_pair(P_GAUGE_WARN, COLOR_YELLOW,  COLOR_BLUE);
    init_pair(P_GAUGE_CRIT, COLOR_RED,     COLOR_BLUE);
    init_pair(P_BAR_DIM,    COLOR_CYAN,    COLOR_BLUE);
}

Dialog::Dialog(int h, int w, const std::string& title) {
    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    if (w > sw - 2) w = sw - 2;
    if (h > sh - 2) h = sh - 2;
    h_ = h;
    w_ = w;
    int y = (sh - h) / 2;
    int x = (sw - w) / 2;

    shadow_win_ = newwin(h, w, y + 1, x + 1);
    wbkgd(shadow_win_, COLOR_PAIR(P_SHADOW));
    shadow_panel_ = new_panel(shadow_win_);

    win_ = newwin(h, w, y, x);
    keypad(win_, TRUE);
    wbkgd(win_, COLOR_PAIR(P_DIALOG));
    // ncursesw ACS line-drawing for native box borders. Uses VT100-compatible
    // ACS constants (mapped to Unicode by ncursesw in UTF-8 locales). Falls
    // back to ASCII on terminals without ACS support.
    box(win_, ACS_VLINE, ACS_HLINE);
    if (!title.empty()) {
        std::string t = " " + title + " ";
        wattron(win_, A_BOLD);
        mvwaddnstr(win_, 0, 2, t.c_str(), w - 4);
        wattroff(win_, A_BOLD);
    }
    panel_ = new_panel(win_);
    update_panels();
    doupdate();
}

Dialog::~Dialog() {
    del_panel(panel_);
    del_panel(shadow_panel_);
    delwin(win_);
    delwin(shadow_win_);
    update_panels();
    doupdate();
}

} // namespace tui
