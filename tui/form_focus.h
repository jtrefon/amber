#ifndef AMBER_TUI_FORM_FOCUS_H
#define AMBER_TUI_FORM_FOCUS_H

// L1 (pure): the key -> intent decision behind the form dialog (form_edit.cpp).
// The shell maps each intent to a form_driver request; no ncurses here.
namespace tui::form_focus {

// Where the keyboard focus is: the editable fields, or one of the two buttons.
enum class Zone { Fields, Ok, Cancel };

// What the shell must do for a key. Requests map one-to-one to form_driver()
// calls, except ToLastField/ToButtons which also move the cursor.
enum class Intent {
    None,
    NextField,
    PrevField,
    PrevChar,
    NextChar,
    BegLine,
    EndLine,
    DelChar,
    DelPrev,
    InsertChar,  // insert `insert` verbatim
    ToButtons,   // leave the fields for the OK button
    ToLastField, // from the buttons back to the last field
    Accept,
    Cancel,
};

struct Decision {
    Intent intent = Intent::None;
    Zone zone = Zone::Fields; // focus zone after this key
    bool done = false;        // the dialog should close
    bool result = false;      // the dialog's return value
    int insert = 0;           // the character for Intent::InsertChar
};

// Decide what a key does. `at_last_field` is whether the focused field is the
// last one (Tab from there leaves the fields for the buttons).
Decision key(int ch, Zone zone, bool at_last_field);

} // namespace tui::form_focus

#endif // AMBER_TUI_FORM_FOCUS_H
