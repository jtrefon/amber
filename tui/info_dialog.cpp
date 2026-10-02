
#include "tui/dialog.h"
#include "tui/info_dialog_layout.h"
#include "widgets.h"

#include <menu.h>

namespace tui {

namespace {

enum class MenuAction { None, Moved, Close };

// The menu over the layout's rows, wired to the dialog window.
MENU* build_menu(WINDOW* w, const info_dialog_layout::Layout& lay, int aw,
                 std::vector<ITEM*>& items) {
    items.reserve(lay.rows.size() + 1);
    for (auto& r : lay.rows)
        items.push_back(new_item(r.c_str(), ""));
    items.push_back(nullptr);

    MENU* menu = new_menu(items.data());
    set_menu_win(menu, w);
    set_menu_sub(menu, derwin(w, lay.list_h, aw - 4, 2, 2));
    set_menu_mark(menu, "");
    set_menu_format(menu, lay.list_h, 1);
    menu_opts_off(menu, O_SHOWDESC);
    set_menu_fore(menu, COLOR_PAIR(P_DIALOG));
    set_menu_back(menu, COLOR_PAIR(P_DIALOG));
    post_menu(menu);
    update_panels();
    doupdate();
    return menu;
}

// Draw the scroll indicators for the menu's current position.
void draw_scroll_hint(MENU* menu, WINDOW* w, const info_dialog_layout::Layout& lay, int aw) {
    const info_dialog_layout::ScrollHint hint =
        info_dialog_layout::scroll_hint(top_row(menu), lay.list_h, static_cast<int>(lay.rows.size()));
    if (!hint.draw)
        return;
    if (hint.up)
        mvwaddch(w, 2, aw - 2, ACS_UARROW);
    if (hint.down)
        mvwaddch(w, lay.list_h + 1, aw - 2, ACS_DARROW);
    update_panels();
    doupdate();
}

MenuAction menu_action(MENU* menu, int c) {
    switch (c) {
    case KEY_DOWN:
        menu_driver(menu, REQ_DOWN_ITEM);
        return MenuAction::Moved;
    case KEY_UP:
        menu_driver(menu, REQ_UP_ITEM);
        return MenuAction::Moved;
    case KEY_NPAGE:
        menu_driver(menu, REQ_SCR_DPAGE);
        return MenuAction::Moved;
    case KEY_PPAGE:
        menu_driver(menu, REQ_SCR_UPAGE);
        return MenuAction::Moved;
    case '\n':
    case '\r':
    case KEY_ENTER:
    case 27:
    case 'q':
    case 'Q':
        return MenuAction::Close;
    default:
        return MenuAction::None;
    }
}

} // namespace

void info_dialog(const std::string& title, const std::vector<std::string>& rows) {
    ModalScope scope;
    curs_set(0);

    int sh, sw;
    getmaxyx(stdscr, sh, sw);
    const info_dialog_layout::Layout lay = info_dialog_layout::compute(rows, title, sh, sw);

    Dialog dlg(lay.height, lay.width, title);
    WINDOW* w = dlg.win();
    const int aw = dlg.cols();
    dlg.set_footer({{"Up/Down", "scroll"}, {"Enter/Esc", "close"}});

    std::vector<ITEM*> items;
    MENU* menu = build_menu(w, lay, aw, items);
    draw_scroll_hint(menu, w, lay, aw);

    bool done = false;
    while (!done) {
        const MenuAction act = menu_action(menu, wgetch(w));
        if (act == MenuAction::Moved)
            draw_scroll_hint(menu, w, lay, aw);
        if (act == MenuAction::Close)
            done = true;
        update_panels();
        doupdate();
    }

    unpost_menu(menu);
    free_menu(menu);
    for (auto* it : items)
        if (it)
            free_item(it);
}

} // namespace tui
