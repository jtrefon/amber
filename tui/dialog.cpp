// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "widgets.h"
#include "textutil.h"

#include <panel.h>

namespace tui {

namespace {
bool* g_modal_flag = nullptr;
} // namespace

void set_modal_flag(bool* flag) { g_modal_flag = flag; }

ModalScope::ModalScope() {
    if (g_modal_flag) *g_modal_flag = true;
}
ModalScope::~ModalScope() {
    if (g_modal_flag) *g_modal_flag = false;
}

void init_pairs() {
    init_pair(P_USER,       COLOR_GREEN,  -1);
    init_pair(P_ASSISTANT,  COLOR_CYAN,   -1);
    init_pair(P_STATUS,     COLOR_YELLOW, -1);
    init_pair(P_DEBUG,      COLOR_MAGENTA, -1);
    init_pair(P_REASONING,  COLOR_WHITE,  -1);
    init_pair(P_BANNER,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_FIELD,      COLOR_WHITE,  COLOR_BLACK);
    init_pair(P_FIELD_ACT,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(P_DIALOG,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_BUTTON,     COLOR_BLACK,  COLOR_WHITE);
    init_pair(P_BUTTON_ACT, COLOR_WHITE, COLOR_YELLOW);
    init_pair(P_SHADOW,     COLOR_BLACK,  COLOR_BLACK);
    init_pair(P_GAUGE_OK,   COLOR_GREEN,   COLOR_BLUE);
    init_pair(P_GAUGE_WARN, COLOR_YELLOW,  COLOR_BLUE);
    init_pair(P_GAUGE_CRIT, COLOR_RED,     COLOR_BLUE);
    init_pair(P_BAR_DIM,    COLOR_CYAN,    COLOR_BLUE);
    init_pair(P_MD_HEAD,    COLOR_WHITE,   -1);
    init_pair(P_MD_QUOTE,   COLOR_CYAN,    -1);
    init_pair(P_MD_CODE,    COLOR_GREEN,   -1);
    init_pair(P_MD_CODEKEY, COLOR_MAGENTA, -1);
    init_pair(P_MD_CODESTR, COLOR_YELLOW,  -1);
    init_pair(P_MD_CODENUM, COLOR_RED,     -1);
    init_pair(P_MD_CODECMT, COLOR_BLUE,    -1);
    init_pair(P_MD_LINK,    COLOR_BLUE,    -1);
    init_pair(P_MD_TABLE,   COLOR_CYAN,    -1);
    init_pair(P_MD_HR,      COLOR_CYAN,    -1);
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

    // Draw border with dialog color pair so it inherits the correct
    // foreground/background. Without this, box() uses A_NORMAL (terminal
    // defaults) and the border may be invisible on some terminals.
    wattr_set(win_, A_NORMAL, P_DIALOG, nullptr);
    // Draw border with Unicode box-drawing characters (avoids ACS fallback
    // to 'q'/'x' on terminals without alternate character set support).
    wborder(win_, L'\u2502', L'\u2502',
                  L'\u2500', L'\u2500',
                  L'\u250c', L'\u2510',
                  L'\u2514', L'\u2518');

    // Title embedded in top border: "┌─ Title ───┐"
    if (!title.empty()) {
        std::wstring t = L" " + text::to_wide(title) + L" ";
        // Erase the top border line where the title goes, then draw it.
        mvwhline(win_, 0, 1, L' ', w_ - 2);
        wattr_set(win_, A_BOLD, P_DIALOG, nullptr);
        mvwaddnwstr(win_, 0, 2, t.c_str(), w_ - 4);
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
