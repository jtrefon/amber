// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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

namespace tui {

// Color pair identifiers used across the whole TUI. Call init_pairs() once,
// after start_color(), to register them.
enum Pair {
    P_USER = 1,     // user text
    P_ASSISTANT,    // assistant text
    P_STATUS,       // tool / status
    P_DEBUG,         // debug trace (magenta)
    P_REASONING,     // model thinking / reasoning (dim)
    P_BANNER,       // banner / status bar
    P_FIELD,        // editable form field (black background)
    P_FIELD_ACT,    // active/focused form field
    P_DIALOG,       // dialog body
    P_BUTTON,       // button (unfocused)
    P_BUTTON_ACT,   // button (focused)
    P_SHADOW,       // drop shadow
    // Status-bar gauge / state segments (colored foreground on the blue bar).
    P_GAUGE_OK,     // context gauge, low pressure   (green on blue)
    P_GAUGE_WARN,   // context gauge, mid pressure    (yellow on blue)
    P_GAUGE_CRIT,   // context gauge, high pressure   (red on blue)
    P_BAR_DIM,      // dim gauge track / faint labels (cyan on blue)
    // Markdown rendering pairs (chat canvas).
    P_MD_HEAD,      // headings
    P_MD_QUOTE,     // block quote
    P_MD_CODE,      // inline code / fenced block
    P_MD_CODEKEY,   // code highlight: keyword
    P_MD_CODESTR,   // code highlight: string
    P_MD_CODENUM,   // code highlight: number
    P_MD_CODECMT,   // code highlight: comment
    P_MD_LINK,      // link text
    P_MD_TABLE,     // table rows
    P_MD_HR,        // horizontal rule
    // Grayscale art pairs (24 levels, 0=black .. 23=near-white).
    P_GRAY = 100,     // grayscale art base (0 + 24 levels)
};

// Register all color pairs. Requires start_color() (and, ideally,
// use_default_colors()) to have been called first.
void init_pairs();

// Register the owner's "a modal dialog is open" flag. The modal primitives
// (info_dialog, menu_select, form_edit) set/clear it so the host event loop can
// defer agent approvals instead of nesting dialogs or deadlocking the worker.
// Pass nullptr to detach.
void set_modal_flag(bool* flag);

// RAII guard: marks the modal flag set on construction, cleared on destruction,
// so a modal primitive always releases it even on early return.
struct ModalScope {
    ModalScope();
    ~ModalScope();
    ModalScope(const ModalScope&) = delete;
    ModalScope& operator=(const ModalScope&) = delete;
};

// Dialog: a centered bordered window managed by a panel, with a drop-shadow
// panel underneath. RAII: constructing shows it, destroying hides/frees it.
class Dialog {
public:
    Dialog(int h, int w, const std::string& title);
    ~Dialog();

    WINDOW* win() const { return win_; }
    int rows() const { return h_; }
    int cols() const { return w_; }

    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

private:
    WINDOW* win_ = nullptr;
    WINDOW* shadow_win_ = nullptr;
    PANEL* panel_ = nullptr;
    PANEL* shadow_panel_ = nullptr;
    int h_ = 0, w_ = 0;
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
