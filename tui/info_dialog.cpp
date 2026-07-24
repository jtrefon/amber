// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/dialog.h"
#include "widgets.h"

#include <menu.h>

#include <algorithm>

namespace tui {

void info_dialog(const std::string& title, const std::vector<std::string>& rows) {
    ModalScope scope;
    curs_set(0);   // hide the input-line cursor while a modal is up
    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    int maxw = 0;
    for (auto& r : rows) maxw = std::max<int>(maxw, static_cast<int>(r.size()));
    int dw = std::min(sw - 4, std::max(maxw + 6, static_cast<int>(title.size()) + 8));
    int dh = std::min(sh - 4, static_cast<int>(rows.size()) + 4);

    Dialog dlg(dh, dw, title);
    WINDOW* w = dlg.win();
    int aw = dlg.cols();

    std::vector<ITEM*> items;
    std::vector<std::string> store = rows;
    for (auto& r : store) {
        if (r.empty()) r = " ";
        items.push_back(new_item(r.c_str(), ""));
    }
    items.push_back(nullptr);

    MENU* menu = new_menu(items.data());
    set_menu_win(menu, w);
    set_menu_sub(menu, derwin(w, dh - 4, aw - 4, 2, 2));
    set_menu_mark(menu, "");
    set_menu_format(menu, dh - 4, 1);
    menu_opts_off(menu, O_SHOWDESC);
    set_menu_fore(menu, COLOR_PAIR(P_DIALOG));
    set_menu_back(menu, COLOR_PAIR(P_DIALOG));
    keypad(w, TRUE);
    post_menu(menu);
    mvwaddnstr(w, dh - 2, 2, "Up/Down scroll  Enter/Esc close", aw - 4);
    update_panels();
    doupdate();

    bool done = false;
    while (!done) {
        int c = wgetch(w);
        switch (c) {
            case KEY_DOWN: menu_driver(menu, REQ_DOWN_ITEM); break;
            case KEY_UP:   menu_driver(menu, REQ_UP_ITEM); break;
            case KEY_NPAGE: menu_driver(menu, REQ_SCR_DPAGE); break;
            case KEY_PPAGE: menu_driver(menu, REQ_SCR_UPAGE); break;
            case '\n': case '\r': case KEY_ENTER:
                case 27: case 'q': case 'Q':
                    done = true; break;
                default:
                    break;
            }
        update_panels();
        doupdate();
    }

    unpost_menu(menu);
    free_menu(menu);
    for (auto* it : items) if (it) free_item(it);
}

} // namespace tui
