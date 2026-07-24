// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_CONFIRM_PANEL_H
#define AMBER_TUI_CONFIRM_PANEL_H

#include "tui/panel.h"

#include <string>

namespace tui {

// Confirmation dialog with Yes/No buttons.
// Returns true if Yes was selected, false on No/Cancel.
class ConfirmPanel : public Panel {
public:
    ConfirmPanel(const std::string& title, const std::string& message);

    bool run();  // true = yes, false = no/cancel

    bool handle_key(int ch) override;

private:
    std::string message_;
    bool yes_selected_ = false;  // true = Yes focused, false = No focused

    void draw_buttons();
};

} // namespace tui

#endif // AMBER_TUI_CONFIRM_PANEL_H
