// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/dialog.h"
#include "widgets.h"

#include <form.h>
#include <menu.h>

#include <algorithm>

namespace tui {

bool form_edit(const std::string& title, std::vector<FieldSpec>& fields) {
    ModalScope scope;
    const int n = static_cast<int>(fields.size());
    const int field_w = 44;
    const int label_w = 16;

    int inner_rows = (n * 2) + 2;
    int dh = inner_rows + 5;
    int dw = label_w + field_w + 8;

    Dialog dlg(dh, dw, title);
    WINDOW* w = dlg.win();
    int aw = dlg.cols();

    std::vector<FIELD*> fs;
    for (int i = 0; i < n; ++i) {
        FIELD* f = new_field(1, field_w, (i * 2) + 2, label_w + 2, 0, 0);
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
        mvwaddnstr(w, (i * 2) + 4, 2, fields[i].label.c_str(), label_w);

    int btn_row = (n * 2) + 5;
    auto draw_buttons = [&](int focus /* 0=fields,1=OK,2=Cancel */) {
        const char* ok = "[  OK  ]";
        const char* cancel = "[ Cancel ]";
        int okx = (aw / 2) - 12;
        int cx = (aw / 2) + 2;
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
                default:
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

} // namespace tui
