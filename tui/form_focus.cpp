#include "tui/form_focus.h"

#include "tui/keys.h"

namespace tui::form_focus {

Decision key(int ch, Zone zone, bool at_last_field) {
    Decision d;
    d.zone = zone;

    if (zone == Zone::Fields) {
        switch (ch) {
        case '\t':
        case keys::kDown:
            if (at_last_field)
                d.zone = Zone::Ok; // Tab off the last field reaches the buttons
            else
                d.intent = Intent::NextField;
            return d;
        case keys::kBtab:
        case keys::kUp:
            d.intent = Intent::PrevField;
            return d;
        case keys::kLeft:
            d.intent = Intent::PrevChar;
            return d;
        case keys::kRight:
            d.intent = Intent::NextChar;
            return d;
        case keys::kHome:
            d.intent = Intent::BegLine;
            return d;
        case keys::kEnd:
            d.intent = Intent::EndLine;
            return d;
        case keys::kDelete:
            d.intent = Intent::DelChar;
            return d;
        case keys::kBackspace:
        case 127:
        case 8:
            d.intent = Intent::DelPrev;
            return d;
        case '\n':
        case '\r':
        case keys::kEnter:
            d.zone = Zone::Ok;
            return d;
        case 27:
            d.done = true;
            d.result = false;
            return d;
        default:
            if (ch >= 32 && ch <= 126) {
                d.intent = Intent::InsertChar;
                d.insert = ch;
            }
            return d;
        }
    }

    // The button row: only OK and Cancel are focusable.
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
        d.result = false;
        return d;
    default:
        return d;
    }
}

} // namespace tui::form_focus
