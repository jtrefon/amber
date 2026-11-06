#ifndef AMBER_TUI_WIDGETS_H
#define AMBER_TUI_WIDGETS_H

// The modal widget API. The dialogs paint through the cell-grid host
// (modal_host.h), not through ncurses windows: a caller keeps this interface,
// the rendering and key loop move to the component layer.

#include "pairs.h"

#include <string>
#include <vector>

namespace tui {

// Register all color pairs. Requires start_color() (and, ideally,
// use_default_colors()) to have been called first.
void init_pairs();

// Register the owner's "a modal dialog is open" flag. A modal primitive sets
// it so the host event loop can defer agent approvals instead of nesting
// dialogs or deadlocking the worker. Pass nullptr to detach.
void set_modal_flag(bool* flag);

// The main event loop ticks getch with this non-blocking timeout. The modal
// host restores this value on every exit path, so a closed dialog cannot freeze
// the tick or make the next one auto-close.
inline constexpr int kTickTimeoutMs = 50;

// RAII guard: marks the modal flag set on construction, cleared on destruction,
// so a modal primitive always releases it even on early return.
struct ModalScope {
    ModalScope();
    ~ModalScope();
    ModalScope(const ModalScope&) = delete;
    ModalScope& operator=(const ModalScope&) = delete;
};

// A single editable field in a form dialog.
struct FieldSpec {
    std::string label;
    std::string value;
    bool secret = false;
};

// Editable fields plus [ OK ]/[ Cancel ] buttons, with Tab/arrow navigation and
// inline editing. Returns true on OK (writing edited values back into `fields`),
// false on Cancel/Esc.
//
// `notice` is drawn just above the buttons. A caller that verifies the values
// (the provider form checks the credential) reopens the same form with the
// reason instead of closing on a value that cannot work.
bool form_edit(const std::string& title, std::vector<FieldSpec>& fields,
               const std::string& notice = {});

// Scrollable read-only text popup. Enter/Esc/q closes.
void info_dialog(const std::string& title, const std::vector<std::string>& rows);

// Chooser. Returns the selected index, or -1 if cancelled.
int menu_select(const std::string& title, const std::vector<std::string>& choices);

} // namespace tui

#endif // AMBER_TUI_WIDGETS_H
