#include "tui/confirm_panel.h"

#include "tui/modal_host.h"

#include <utility>

namespace tui {

ApprovalPanel::ApprovalPanel(std::string summary, int timeout_sec, int default_idx)
    : summary_(std::move(summary)), timeout_sec_(timeout_sec), default_idx_(default_idx) {}

agent::Approval ApprovalPanel::run() {
    return ModalHost{}.approve(summary_, timeout_sec_, default_idx_);
}

agent::Approval approve_dialog(const std::string& summary, int timeout_sec, int default_idx) {
    return ModalHost{}.approve(summary, timeout_sec, default_idx);
}

ConfirmPanel::ConfirmPanel(std::string title, std::string message)
    : title_(std::move(title)), message_(std::move(message)) {}

bool ConfirmPanel::run() {
    return ModalHost{}.confirm(title_, message_);
}

} // namespace tui
