#ifndef AMBER_TUI_LIST_PANEL_H
#define AMBER_TUI_LIST_PANEL_H

#include <string>
#include <vector>

namespace tui {

// A titled chooser. The selection/filter/scroll model and the painting live in
// the modal host now; this class keeps the call-site API.
class ListPanel {
public:
    ListPanel(std::string title, std::vector<std::string> items);

    int run(); // Returns selected index, or -1 on cancel

private:
    std::string title_;
    std::vector<std::string> items_;
};

} // namespace tui

#endif // AMBER_TUI_LIST_PANEL_H
