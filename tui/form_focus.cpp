#include "tui/form_focus.h"

#include "tui/keys.h"

namespace tui::form_focus {

namespace {

// Movement keys map one-to-one to an intent; a table keeps the field switch to
// the keys that need more than an intent (zone moves, commit, escape).
Intent movement_intent(int ch) noexcept {
    switch (ch) {
    case keys::kLeft:
        return Intent::PrevChar;
    case keys::kRight:
        return Intent::NextChar;
    case keys::kHome:
        return Intent::BegLine;
    case keys::kEnd:
        return Intent::EndLine;
    case keys::kDelete:
        return Intent::DelChar;
    default:
        return Intent::None;
    }
}

bool is_backspace(int ch) noexcept {
    return ch == keys::kBackspace || ch == 127 || ch == 8;
}

bool is_submit(int ch) noexcept {
    return ch == '\n' || ch == '\r' || ch == keys::kEnter;
}

// Keys while a field has focus. The returned decision keeps Zone::Fields
// unless the key moves the focus out.
Decision field_key(int ch, bool at_last_field) {
    Decision d;
    const Intent nav = movement_intent(ch);
    if (nav != Intent::None) {
        d.intent = nav;
        return d;
    }
    if (ch == '\t' || ch == keys::kDown) {
        if (at_last_field)
            d.zone = Zone::Ok; // Tab off the last field reaches the buttons
        else
            d.intent = Intent::NextField;
        return d;
    }
    if (ch == keys::kBtab || ch == keys::kUp) {
        d.intent = Intent::PrevField;
        return d;
    }
    if (is_backspace(ch)) {
        d.intent = Intent::DelPrev;
        return d;
    }
    if (is_submit(ch)) {
        d.zone = Zone::Ok;
        return d;
    }
    if (ch == 27) {
        d.done = true;
        return d;
    }
    if (ch >= 32 && ch <= 126) {
        d.intent = Intent::InsertChar;
        d.insert = ch;
    }
    return d;
}

// The button row: only OK and Cancel are focusable.
Decision button_key(int ch, Zone zone) {
    Decision d;
    d.zone = zone;
    switch (ch) {
    case '\t':
    case keys::kRight:
    case keys::kBtab:
    case keys::kLeft:
        d.zone = (zone == Zone::Ok) ? Zone::Cancel : Zone::Ok;
        return d;
    case keys::kUp:
        d.zone = Zone::Fields;
        d.intent = Intent::ToLastField;
        return d;
    case '\n':
    case '\r':
    case keys::kEnter:
        d.intent = Intent::Accept;
        d.done = true;
        d.result = (zone == Zone::Ok);
        return d;
    case 27:
        d.done = true;
        return d;
    default:
        return d;
    }
}

} // namespace

Decision key(int ch, Zone zone, bool at_last_field) {
    if (zone == Zone::Fields)
        return field_key(ch, at_last_field);
    return button_key(ch, zone);
}

} // namespace tui::form_focus
