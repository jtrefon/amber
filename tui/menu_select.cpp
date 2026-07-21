// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "widgets.h"

#include <menu.h>

#include <algorithm>

namespace tui {

int menu_select(const std::string& title, const std::vector<std::string>& choices) {
    ModalScope scope;
    curs_set(0);   // hide the input-line cursor while a modal is up
    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    int maxw = static_cast<int>(title.size());
    for (auto& c : choices) maxw = std::max<int>(maxw, static_cast<int>(c.size()));
    int dw = std::min(sw - 4, maxw + 10);
    int dh = std::min(sh - 4, static_cast<int>(choices.size()) + 5);

    Dialog dlg(dh, dw, title);
    WINDOW* w = dlg.win();
    int aw = dlg.cols();

    std::vector<ITEM*> items;
    items.reserve(choices.size());
for (auto& c : choices) items.push_back(new_item(c.c_str(), ""));
    items.push_back(nullptr);

    MENU* menu = new_menu(items.data());
    set_menu_win(menu, w);
    set_menu_sub(menu, derwin(w, dh - 4, aw - 4, 2, 2));
    set_menu_mark(menu, " > ");
    menu_opts_off(menu, O_SHOWDESC);
    set_menu_fore(menu, COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
    set_menu_back(menu, COLOR_PAIR(P_DIALOG));
    keypad(w, TRUE);
    post_menu(menu);
    mvwaddnstr(w, dh - 2, 2, "Up/Down move  Enter select  Esc cancel", aw - 4);
    update_panels();
    doupdate();

    int result = -1;
    bool done = false;
    while (!done) {
        int c = wgetch(w);
        switch (c) {
            case KEY_DOWN: case '\t': menu_driver(menu, REQ_DOWN_ITEM); break;
            case KEY_UP: case KEY_BTAB: menu_driver(menu, REQ_UP_ITEM); break;
            case '\n': case '\r': case KEY_ENTER:
                result = item_index(current_item(menu));
                done = true;
                break;
            case 27: case 'q':
                result = -1; done = true; break;
            default:
                break;
        }
        update_panels();
        doupdate();
    }

    unpost_menu(menu);
    free_menu(menu);
    for (auto* it : items) if (it) free_item(it);
    return result;
}

} // namespace tui
