
#include "tui/dialog.h"
#include "tui/form_focus.h"
#include "widgets.h"

#include <iterator>
#include <form.h>
#include <menu.h>

#include <algorithm>

namespace tui {

// The ncurses FORM and its sub-window; both must outlive the event loop.
struct FormParts {
    FORM* form = nullptr;
    WINDOW* sub = nullptr;
};

// One FIELD per spec, plus the null terminator new_form() requires.
std::vector<FIELD*> create_fields(const std::vector<FieldSpec>& fields, int field_w, int label_w) {
    std::vector<FIELD*> fs;
    fs.reserve(fields.size() + 1);
    for (size_t i = 0; i < fields.size(); ++i) {
        const int row = (static_cast<int>(i) * 2) + 2;
        FIELD* f = new_field(1, field_w, row, label_w + 2, 0, 0);
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
    return fs;
}

FormParts build_form(std::vector<FIELD*>& fs, WINDOW* w) {
    FormParts parts;
    parts.form = new_form(fs.data());
    int frows = 0;
    int fcols = 0;
    scale_form(parts.form, &frows, &fcols);
    set_form_win(parts.form, w);
    parts.sub = derwin(w, frows, fcols, 2, 2);
    set_form_sub(parts.form, parts.sub);
    post_form(parts.form);
    return parts;
}

void draw_field_labels(WINDOW* w, const std::vector<FieldSpec>& fields, int label_w) {
    for (size_t i = 0; i < fields.size(); ++i)
        mvwaddnstr(w, (static_cast<int>(i) * 2) + 4, 2, fields[i].label.c_str(), label_w);
}

// focus: 0 = fields, 1 = OK, 2 = Cancel.
void draw_form_buttons(WINDOW* w, int aw, int btn_row, int focus) {
    const char* ok = "[  OK  ]";
    const char* cancel = "[ Cancel ]";
    const int okx = (aw / 2) - 12;
    const int cx = (aw / 2) + 2;
    wattron(w, COLOR_PAIR(focus == 1 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
    mvwaddstr(w, btn_row, okx, ok);
    wattroff(w, COLOR_PAIR(focus == 1 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
    wattron(w, COLOR_PAIR(focus == 2 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
    mvwaddstr(w, btn_row, cx, cancel);
    wattroff(w, COLOR_PAIR(focus == 2 ? P_BUTTON_ACT : P_BUTTON) | A_BOLD);
}

// The cursor is visible only while the fields hold focus.
void draw_form_cursor(FORM* form, int focus) {
    if (focus != 0) {
        curs_set(0);
        return;
    }
    curs_set(1);
    pos_form_cursor(form);
}

// Drive the form for whatever the key layer decided.
namespace {

// Each intent that maps to a plain driver request, as data. Intent is a contiguous
// enum, so this is a table indexed by intent rather than a chain of eleven
// near-identical branches. `trailing` is the second request a few intents need, or
// 0. kElsewhere marks the intents this function does not serve: InsertChar's
// request comes from the decision itself, and ToLastField also moves the field
// cursor, so both are handled before the table.
// ncurses form request codes are plain int macros (REQ_* in <form.h>), not a
// named type, so the table is int.
constexpr int kElsewhere = 0;

struct Step {
    int primary;
    int trailing;
};

constexpr Step kSteps[] = {
    {kElsewhere, 0},                // None
    {REQ_NEXT_FIELD, REQ_END_LINE}, // NextField -- field moves, then end the line
    {REQ_PREV_FIELD, REQ_END_LINE}, // PrevField
    {REQ_PREV_CHAR, 0},             // PrevChar
    {REQ_NEXT_CHAR, 0},             // NextChar
    {REQ_BEG_LINE, 0},              // BegLine
    {REQ_END_LINE, 0},              // EndLine
    {REQ_DEL_CHAR, 0},              // DelChar
    {REQ_DEL_PREV, 0},              // DelPrev
    {kElsewhere, 0},                // InsertChar -- request comes from the decision
    {kElsewhere, 0},                // ToButtons
    {kElsewhere, 0},                // ToLastField -- also moves the field cursor
    {kElsewhere, 0},                // Accept
    {kElsewhere, 0},                // Cancel
};
static_assert(std::size(kSteps) == static_cast<size_t>(form_focus::Intent::Cancel) + 1,
              "every form_focus::Intent needs a row, or the table is indexed wrong");

} // namespace

void apply_form_intent(FORM* form, const form_focus::Decision& d, std::vector<FIELD*>& fs, int n) {
    if (d.intent == form_focus::Intent::InsertChar) {
        form_driver(form, d.insert);
        return;
    }
    if (d.intent == form_focus::Intent::ToLastField) {
        set_current_field(form, fs[n - 1]);
        form_driver(form, REQ_END_LINE);
        return;
    }
    const auto index = static_cast<size_t>(d.intent);
    if (index >= std::size(kSteps) || kSteps[index].primary == kElsewhere)
        return;
    form_driver(form, kSteps[index].primary);
    if (kSteps[index].trailing != 0)
        form_driver(form, kSteps[index].trailing);
}

// Copy the edited buffers back, trimmed of the field's trailing padding.
void collect_field_values(FORM* form, std::vector<FIELD*>& fs, std::vector<FieldSpec>& fields) {
    form_driver(form, REQ_VALIDATION);
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string v = field_buffer(fs[i], 0);
        const size_t end = v.find_last_not_of(' ');
        fields[i].value = (end == std::string::npos) ? "" : v.substr(0, end + 1);
    }
}

void destroy_form(FormParts& parts, std::vector<FIELD*>& fs) {
    curs_set(0);
    unpost_form(parts.form);
    free_form(parts.form);
    for (FIELD* f : fs)
        if (f)
            free_field(f);
    if (parts.sub)
        delwin(parts.sub);
}

// The modal loop: draw the buttons and cursor, read a key, route it through
// form_focus, and apply the verdict. Returns the form's result.
bool run_form_loop(WINDOW* w, FORM* form, int aw, int btn_row, int n, std::vector<FIELD*>& fs) {
    form_focus::Zone zone = form_focus::Zone::Fields;
    curs_set(1);
    set_current_field(form, fs[0]);
    form_driver(form, REQ_END_LINE);

    bool result = false;
    bool done = false;
    while (!done) {
        const int focus =
            (zone == form_focus::Zone::Fields) ? 0 : (zone == form_focus::Zone::Ok ? 1 : 2);
        draw_form_buttons(w, aw, btn_row, focus);
        draw_form_cursor(form, focus);
        update_panels();
        doupdate();

        const int c = wgetch(w);
        const bool at_last_field = (field_index(current_field(form)) == n - 1);
        const form_focus::Decision d = form_focus::key(c, zone, at_last_field);
        zone = d.zone;
        apply_form_intent(form, d, fs, n);
        if (d.done) {
            result = d.result;
            done = true;
        }
    }
    return result;
}

bool form_edit(const std::string& title, std::vector<FieldSpec>& fields) {
    ModalScope scope;
    const int n = static_cast<int>(fields.size());
    const int field_w = 44;
    const int label_w = 16;
    const int btn_row = (n * 2) + 5;

    Dialog dlg((n * 2) + 2 + 5, label_w + field_w + 8, title);
    WINDOW* w = dlg.win();
    const int aw = dlg.cols();

    std::vector<FIELD*> fs = create_fields(fields, field_w, label_w);
    FormParts form = build_form(fs, w);
    draw_field_labels(w, fields, label_w);
    dlg.set_footer({{"Tab/Arrows", "move"}, {"Enter", "confirm"}, {"Esc", "cancel"}});

    const bool result = run_form_loop(w, form.form, aw, btn_row, n, fs);

    if (result)
        collect_field_values(form.form, fs, fields);
    destroy_form(form, fs);
    return result;
}

} // namespace tui
