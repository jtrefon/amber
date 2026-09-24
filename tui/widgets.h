
#ifndef AMBER_TUI_WIDGETS_H
#define AMBER_TUI_WIDGETS_H

// Reusable native-ncurses widget layer for the TUI. All modal subcomponents
// (settings form, menus, info popups) are built on top of these so that new
// screens can be added without re-implementing input handling, borders,
// shadows, or navigation.

#include <ncurses.h>
#include <panel.h>

#include <string>
#include <vector>

#include "pairs.h"

namespace tui {

// Register all color pairs. Requires start_color() (and, ideally,
// use_default_colors()) to have been called first.
void init_pairs();

// Register the owner's "a modal dialog is open" flag. The modal primitives
// (info_dialog, menu_select, form_edit) set/clear it so the host event loop can
// defer agent approvals instead of nesting dialogs or deadlocking the worker.
// Pass nullptr to detach.
void set_modal_flag(bool* flag);

// The main event loop ticks getch with this non-blocking timeout. Modal
// dialogs that read stdscr switch to blocking and MUST restore this value on
// every exit path — a dialog that leaves the timeout at -1 freezes the tick
// (streaming render, spinners, countdown); a dialog that leaves it at 50 ms
// auto-closes after 50 ms without a keypress. Tui::run() is the single
// production writer; keep the two in sync.
inline constexpr int kTickTimeoutMs = 50;

// RAII: restores the main-loop tick timeout when a stdscr-modal exits.
// Non-copyable/non-movable (like ModalScope) so the timeout can never be
// restored twice.
struct BlockingInputGuard {
    BlockingInputGuard() = default;
    ~BlockingInputGuard() { timeout(kTickTimeoutMs); }
    BlockingInputGuard(const BlockingInputGuard&) = delete;
    BlockingInputGuard& operator=(const BlockingInputGuard&) = delete;
    BlockingInputGuard(BlockingInputGuard&&) = delete;
    BlockingInputGuard& operator=(BlockingInputGuard&&) = delete;
};

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

// Native libform modal. Editable fields plus [ OK ]/[ Cancel ] buttons, with
// Tab/arrow navigation and inline editing. Returns true on OK (writing edited
// values back into `fields`), false on Cancel/Esc.
bool form_edit(const std::string& title, std::vector<FieldSpec>& fields);

// Scrollable read-only text popup (libmenu-backed). Enter/Esc/q closes.
void info_dialog(const std::string& title, const std::vector<std::string>& rows);

// Native libmenu chooser. Returns the selected index, or -1 if cancelled.
int menu_select(const std::string& title, const std::vector<std::string>& choices);

} // namespace tui

#endif // AMBER_TUI_WIDGETS_H
