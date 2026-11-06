#ifndef AMBER_TUI_CONFIRM_PANEL_H
#define AMBER_TUI_CONFIRM_PANEL_H

#include <agent/agent.h>

#include <string>

namespace tui {

// Approval dialog: four options with a countdown, rendered by the modal host.
// Returns the selected Approval value.
class ApprovalPanel {
public:
    ApprovalPanel(std::string summary, int timeout_sec, int default_idx);

    agent::Approval run();

private:
    std::string summary_;
    int timeout_sec_;
    int default_idx_;
};

// Convenience wrapper: shows the approval dialog and returns the decision.
// If timeout_sec == 0 the dialog waits indefinitely.
// default_idx 0-3 selects the initially highlighted option.
agent::Approval approve_dialog(const std::string& summary, int timeout_sec = 60,
                               int default_idx = 0);

// Simple yes/no confirmation dialog.
class ConfirmPanel {
public:
    ConfirmPanel(std::string title, std::string message);
    bool run();

private:
    std::string title_;
    std::string message_;
};

} // namespace tui

#endif // AMBER_TUI_CONFIRM_PANEL_H
