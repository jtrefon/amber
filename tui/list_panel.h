// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_LIST_PANEL_H
#define AMBER_TUI_LIST_PANEL_H

#include "tui/panel.h"

#include <string>
#include <vector>

namespace tui {

// Scrollable selection list panel. Items are displayed with a highlight bar.
// Keyboard: Up/Down to navigate, Enter to select, Esc to cancel.
class ListPanel : public Panel {
public:
    ListPanel(const std::string& title, const std::vector<std::string>& items,
              std::vector<FooterKey> footer = {});

    int run();  // Returns selected index, or -1 on cancel

    bool handle_key(int ch) override;

private:
    std::vector<std::string> items_;
    std::string filter_;
    int selection_ = 0;
    int scroll_offset_ = 0;
    bool filter_mode_ = false;

    std::vector<std::string> filtered() const;
    void draw_items();
    void draw_filter_bar();
};

} // namespace tui

#endif // AMBER_TUI_LIST_PANEL_H
