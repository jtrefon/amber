
#include "tui/dialog.h"
#include "tui/form_focus.h"
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
        field_opts_off(f, O_STATIC); // allow horizontal scrolling
        set_max_field(f, 1024);
        if (fields[i].secret)
            field_opts_off(f, O_PUBLIC);
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

    dlg.set_footer({{"Tab/Arrows", "move"}, {"Enter", "confirm"}, {"Esc", "cancel"}});
    form_focus::Zone zone = form_focus::Zone::Fields;
    curs_set(1);
    set_current_field(form, fs[0]);
    form_driver(form, REQ_END_LINE);

    bool result = false;
    bool done = false;
    while (!done) {
        const int focus =
            (zone == form_focus::Zone::Fields) ? 0 : (zone == form_focus::Zone::Ok ? 1 : 2);
        draw_buttons(focus);
        if (focus == 0) {
            curs_set(1);
            pos_form_cursor(form);
        } else {
            curs_set(0);
        }
        update_panels();
        doupdate();

        const int c = wgetch(w);
        const bool at_last_field = (field_index(current_field(form)) == n - 1);
        const form_focus::Decision d = form_focus::key(c, zone, at_last_field);
        zone = d.zone;
        switch (d.intent) {
        case form_focus::Intent::NextField:
            form_driver(form, REQ_NEXT_FIELD);
            form_driver(form, REQ_END_LINE);
            break;
        case form_focus::Intent::PrevField:
            form_driver(form, REQ_PREV_FIELD);
            form_driver(form, REQ_END_LINE);
            break;
        case form_focus::Intent::PrevChar:
            form_driver(form, REQ_PREV_CHAR);
            break;
        case form_focus::Intent::NextChar:
            form_driver(form, REQ_NEXT_CHAR);
            break;
        case form_focus::Intent::BegLine:
            form_driver(form, REQ_BEG_LINE);
            break;
        case form_focus::Intent::EndLine:
            form_driver(form, REQ_END_LINE);
            break;
        case form_focus::Intent::DelChar:
            form_driver(form, REQ_DEL_CHAR);
            break;
        case form_focus::Intent::DelPrev:
            form_driver(form, REQ_DEL_PREV);
            break;
        case form_focus::Intent::InsertChar:
            form_driver(form, d.insert);
            break;
        case form_focus::Intent::ToLastField:
            set_current_field(form, fs[n - 1]);
            form_driver(form, REQ_END_LINE);
            break;
        default:
            break;
        }
        if (d.done) {
            result = d.result;
            done = true;
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
    for (int i = 0; i < n; ++i)
        free_field(fs[i]);
    if (fsub)
        delwin(fsub);
    return result;
}

} // namespace tui
