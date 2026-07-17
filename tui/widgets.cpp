// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "widgets.h"

#include <form.h>
#include <menu.h>

#include <algorithm>

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
    // Use ASCII borders rather than ACS line-drawing chars: some terminals
    // (PuTTY with certain translations, minimal TERMs) render ACS as garbage.
    // '+' corners, '-' top/bottom, '|' sides are universally safe.
    wborder(win_, '|', '|', '-', '-', '+', '+', '+', '+');
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

bool form_edit(const std::string& title, std::vector<FieldSpec>& fields) {
    const int n = static_cast<int>(fields.size());
    const int field_w = 44;
    const int label_w = 16;

    int inner_rows = n * 2 + 2;
    int dh = inner_rows + 5;
    int dw = label_w + field_w + 8;

    Dialog dlg(dh, dw, title);
    WINDOW* w = dlg.win();
    int aw = dlg.cols();

    std::vector<FIELD*> fs;
    for (int i = 0; i < n; ++i) {
        FIELD* f = new_field(1, field_w, i * 2 + 2, label_w + 2, 0, 0);
        set_field_back(f, COLOR_PAIR(P_FIELD));
        set_field_fore(f, COLOR_PAIR(P_FIELD));
        field_opts_off(f, O_AUTOSKIP);
        field_opts_off(f, O_STATIC);          // allow horizontal scrolling
        set_max_field(f, 1024);
        if (fields[i].secret) field_opts_off(f, O_PUBLIC);
        set_field_buffer(f, 0, fields[i].value.c_str());
        fs.push_back(f);
    }
    fs.push_back(nullptr);

    FORM* form = new_form(fs.data());
    int frows, fcols;
    scale_form(form, &frows, &fcols);
    set_form_win(form, w);
    WINDOW* fsub = derwin(w, frows, fcols, 2, 2);
    set_form_sub(form, fsub);
    keypad(w, TRUE);
    post_form(form);

    for (int i = 0; i < n; ++i)
        mvwaddnstr(w, i * 2 + 4, 2, fields[i].label.c_str(), label_w);

    int btn_row = n * 2 + 5;
    auto draw_buttons = [&](int focus /* 0=fields,1=OK,2=Cancel */) {
        const char* ok = "[  OK  ]";
        const char* cancel = "[ Cancel ]";
        int okx = aw / 2 - 12;
        int cx = aw / 2 + 2;
        wattron(w, COLOR_PAIR(focus == 1 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
        mvwaddstr(w, btn_row, okx, ok);
        wattroff(w, COLOR_PAIR(focus == 1 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
        wattron(w, COLOR_PAIR(focus == 2 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
        mvwaddstr(w, btn_row, cx, cancel);
        wattroff(w, COLOR_PAIR(focus == 2 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
    };

    mvwaddnstr(w, dh - 2, 2,
               "Tab/Arrows move  Enter confirm  Esc cancel", aw - 4);

    int focus = 0;
    curs_set(1);
    set_current_field(form, fs[0]);
    form_driver(form, REQ_END_LINE);

    bool result = false;
    bool done = false;
    while (!done) {
        draw_buttons(focus);
        if (focus == 0) {
            curs_set(1);
            pos_form_cursor(form);
        } else {
            curs_set(0);
        }
        update_panels();
        doupdate();

        int c = wgetch(w);
        if (focus == 0) {
            switch (c) {
                case '\t':
                case KEY_DOWN:
                    if (field_index(current_field(form)) == n - 1) {
                        focus = 1;
                    } else {
                        form_driver(form, REQ_NEXT_FIELD);
                        form_driver(form, REQ_END_LINE);
                    }
                    break;
                case KEY_BTAB:
                case KEY_UP:
                    form_driver(form, REQ_PREV_FIELD);
                    form_driver(form, REQ_END_LINE);
                    break;
                case KEY_LEFT:  form_driver(form, REQ_PREV_CHAR); break;
                case KEY_RIGHT: form_driver(form, REQ_NEXT_CHAR); break;
                case KEY_HOME:  form_driver(form, REQ_BEG_LINE); break;
                case KEY_END:   form_driver(form, REQ_END_LINE); break;
                case KEY_DC:    form_driver(form, REQ_DEL_CHAR); break;
                case KEY_BACKSPACE:
                case 127:
                case 8:         form_driver(form, REQ_DEL_PREV); break;
                case '\n':
                case '\r':
                case KEY_ENTER: focus = 1; break;
                case 27:        result = false; done = true; break;
                default:
                    if (c >= 32 && c <= 126) form_driver(form, c);
                    break;
            }
        } else {
            switch (c) {
                case '\t':
                case KEY_RIGHT:
                case KEY_BTAB:
                case KEY_LEFT:
                    focus = (focus == 1) ? 2 : 1;
                    break;
                case KEY_UP:
                    focus = 0;
                    set_current_field(form, fs[n - 1]);
                    form_driver(form, REQ_END_LINE);
                    break;
                case '\n':
                case '\r':
                case KEY_ENTER:
                    result = (focus == 1);
                    done = true;
                    break;
                case 27:
                    result = false;
                    done = true;
                    break;
            }
        }
    }

    if (result) {
        form_driver(form, REQ_VALIDATION);
        for (int i = 0; i < n; ++i) {
            std::string v = field_buffer(fs[i], 0);
            size_t end = v.find_last_not_of(' ');
            fields[i].value = (end == std::string::npos) ? "" : v.substr(0, end + 1);
        }
    }

    curs_set(0);
    unpost_form(form);
    free_form(form);
    for (int i = 0; i < n; ++i) free_field(fs[i]);
    if (fsub) delwin(fsub);
    return result;
}

void info_dialog(const std::string& title, const std::vector<std::string>& rows) {
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
        }
        update_panels();
        doupdate();
    }

    unpost_menu(menu);
    free_menu(menu);
    for (auto* it : items) if (it) free_item(it);
}

int menu_select(const std::string& title, const std::vector<std::string>& choices) {
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
