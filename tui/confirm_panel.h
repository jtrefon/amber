
#ifndef AMBER_TUI_CONFIRM_PANEL_H
#define AMBER_TUI_CONFIRM_PANEL_H

#include "tui/panel.h"

#include <agent/agent.h>

#include <string>

namespace tui {

class ApprovalModel;

// Approval dialog: 4 options with countdown timer and keyboard shortcuts.
// Returns the selected Approval value.
class ApprovalPanel : public Panel {
public:
    ApprovalPanel(const std::string& summary,
                  int timeout_sec,
                  int default_idx);

    agent::Approval run();

private:
    // Key dispatch for the dialog loop; returns true when the dialog should
    // close. The model is the single source of truth for the selection.
    bool handle_dialog_key(int ch, ApprovalModel& model);
    std::string summary_;
    int timeout_sec_;
    int sel_ = 0;           // 0=AllowOnce, 1=AllowSession, 2=AlwaysAllow, 3=AlwaysDeny
    int remaining_ = 0;

    void draw();
    void draw_button(int i, int y, bool selected, bool has_timer);
    const char* label(int i) const;
};

// Convenience wrapper: shows the approval dialog and returns the decision.
// If timeout_sec == 0 the dialog waits indefinitely.
// default_idx 0-3 selects the initially highlighted option.
agent::Approval approve_dialog(const std::string& summary,
                                int timeout_sec = 60,
                                int default_idx = 0);

// Simple yes/no confirmation dialog.
class ConfirmPanel : public Panel {
public:
    ConfirmPanel(const std::string& title, const std::string& message);
    bool run();
private:
    std::string message_;
    bool yes_ = false;
    void draw_buttons();
};

} // namespace tui

#endif // AMBER_TUI_CONFIRM_PANEL_H
