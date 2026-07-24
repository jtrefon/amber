// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/dialog.h"
#include "widgets.h"

#include <menu.h>

#include <algorithm>

namespace tui {

void info_dialog(const std::string& title, const std::vector<std::string>& rows) {
    ModalScope scope;
    curs_set(0);

    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    int maxw = 0;
    for (auto& r : rows) maxw = std::max<int>(maxw, static_cast<int>(r.size()));
    int dw = std::min(sw - 4, std::max(maxw + 6, static_cast<int>(title.size()) + 8));
    int dh = std::min(sh - 4, static_cast<int>(rows.size()) + 4);
    int list_h = dh - 4;  // rows available for the menu

    Dialog dlg(dh, dw, title);
    WINDOW* w = dlg.win();
    int aw = dlg.cols();
    dlg.set_footer({{"Up/Down", "scroll"}, {"Enter/Esc", "close"}});

    std::vector<ITEM*> items;
    std::vector<std::string> store = rows;
    for (auto& r : store) {
        if (r.empty()) r = " ";
        items.push_back(new_item(r.c_str(), ""));
    }
    items.push_back(nullptr);

    MENU* menu = new_menu(items.data());
    set_menu_win(menu, w);
    set_menu_sub(menu, derwin(w, list_h, aw - 4, 2, 2));
    set_menu_mark(menu, "");
    set_menu_format(menu, list_h, 1);
    menu_opts_off(menu, O_SHOWDESC);
    set_menu_fore(menu, COLOR_PAIR(P_DIALOG));
    set_menu_back(menu, COLOR_PAIR(P_DIALOG));
    keypad(w, TRUE);
    post_menu(menu);
    update_panels();
    doupdate();

    // Draw scroll indicators
    auto draw_scroll = [&]() {
        int top_item = top_row(menu);
        int visible = list_h;
        int total = static_cast<int>(rows.size());
        if (total <= visible) return;
        if (top_item > 0)
            mvwaddch(w, 2, aw - 2, ACS_UARROW);
        if (top_item + visible < total)
            mvwaddch(w, list_h + 1, aw - 2, ACS_DARROW);
        update_panels();
        doupdate();
    };
    draw_scroll();

    bool done = false;
    while (!done) {
        int c = wgetch(w);
        switch (c) {
            case KEY_DOWN: menu_driver(menu, REQ_DOWN_ITEM); draw_scroll(); break;
            case KEY_UP:   menu_driver(menu, REQ_UP_ITEM); draw_scroll(); break;
            case KEY_NPAGE: menu_driver(menu, REQ_SCR_DPAGE); draw_scroll(); break;
            case KEY_PPAGE: menu_driver(menu, REQ_SCR_UPAGE); draw_scroll(); break;
            case '\n': case '\r': case KEY_ENTER:
            case 27: case 'q': case 'Q':
                done = true; break;
            default: break;
        }
        update_panels();
        doupdate();
    }

    unpost_menu(menu);
    free_menu(menu);
    for (auto* it : items) if (it) free_item(it);
}

} // namespace tui
