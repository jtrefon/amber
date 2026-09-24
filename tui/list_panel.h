#ifndef AMBER_TUI_LIST_PANEL_H
#define AMBER_TUI_LIST_PANEL_H

#include "tui/list_state.h"
#include "tui/panel.h"

#include <string>
#include <vector>

namespace tui {

// Scrollable selection list panel. Items are displayed with a highlight bar.
// Keyboard: Up/Down to navigate, Enter to select, Esc to cancel. The
// selection/filter/scroll model is pure (ListState); this class paints it.
class ListPanel : public Panel {
public:
    ListPanel(const std::string& title, const std::vector<std::string>& items);
    ListPanel(const std::string& title, const std::vector<std::string>& items,
              std::vector<FooterKey> footer);

    int run(); // Returns selected index, or -1 on cancel

private:
    bool handle_key(int ch) override;
    void run_input_loop();

    ListState state_;
    void draw_items();
    void draw_filter_bar();
};

} // namespace tui

#endif // AMBER_TUI_LIST_PANEL_H
