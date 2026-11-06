#ifndef AMBER_TUI_DIALOG_VIEW_H
#define AMBER_TUI_DIALOG_VIEW_H

// The dialog painter: one view-model in, one framed Surface out. The host blits
// the surface over the terminal, so this file never sees a terminal or a key.
// Everything the view needs is already decided by the state machines.

#include "tui/geometry.h"
#include "tui/surface.h"
#include "tui/widgets.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tui {

struct DialogView {
    std::string title;
    std::string footer;
    std::string notice;
    std::vector<std::string> lines;   // message / info rows (already sliced)
    std::vector<std::string> items;   // list rows (already filtered)
    std::vector<FieldSpec> fields;    // form rows
    std::vector<std::string> buttons; // e.g. {"[ Yes ]", "[ No ]"}
    std::string filter;
    bool filter_mode = false;
    int selected_item = 0;
    int first_item = 0;
    int selected_button = -1;
    int focused_field = -1;
    std::size_t field_cursor = 0;
    bool scroll_up = false;
    bool scroll_down = false;
};

struct PaintedDialog {
    Surface surface;
    int x = 0;
    int y = 0;
    int cursor_x = -1; // -1 hides the terminal cursor
    int cursor_y = -1;
};

PaintedDialog paint_dialog(const DialogView& view, Size screen);

} // namespace tui

#endif // AMBER_TUI_DIALOG_VIEW_H
