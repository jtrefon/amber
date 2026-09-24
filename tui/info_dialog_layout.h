#ifndef AMBER_TUI_INFO_DIALOG_LAYOUT_H
#define AMBER_TUI_INFO_DIALOG_LAYOUT_H

#include <string>
#include <vector>

// L1 (pure): geometry and viewport hints for the info popup. The ncurses MENU
// shell (info_dialog.cpp) only paints what this computes.
namespace tui::info_dialog_layout {

struct Layout {
    int width = 0;                 // dialog width
    int height = 0;                // dialog height
    int list_h = 0;                // rows available for the menu
    std::vector<std::string> rows; // menu text; an empty row becomes " "
};

// Dialog geometry and menu text for a screen of `screen_h` x `screen_w`.
Layout compute(const std::vector<std::string>& rows, const std::string& title, int screen_h,
               int screen_w);

// Which scroll indicators the menu viewport needs. `draw` is false when the
// content fits, so the caller can skip the repaint entirely.
struct ScrollHint {
    bool draw = false;
    bool up = false;
    bool down = false;
};
ScrollHint scroll_hint(int top_item, int visible, int total);

} // namespace tui::info_dialog_layout

#endif // AMBER_TUI_INFO_DIALOG_LAYOUT_H
