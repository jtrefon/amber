#include "checkpoint_commands.h"

#include "confirm_panel.h"
#include "tui.h"

#include "agent/checkpoint_format.h"
#include "agent/workspace.h"

#include <sstream>
#include <string>
#include <vector>

namespace tui {

namespace {

constexpr std::size_t kMaxDiff = std::size_t{16} * 1024;

} // namespace

bool CheckpointCommands::require_chat() const {
    if (ui_.win().kind == Window::Kind::Chat)
        return true;
    ui_.append_line(P_STATUS, "checkpoints: the active window is not a chat session");
    ui_.draw();
    return false;
}

void CheckpointCommands::report_restore(const agent::CheckpointChange& change, const char* verb) {
    std::string line = std::string("checkpoint ") + verb + ": restored " +
                       agent::checkpoint_describe(change.target);
    if (!change.checkpoint.id.empty())
        line += " — recorded " + agent::checkpoint_short_id(change.checkpoint);
    else
        line += " (the tree was already there)";
    ui_.append_line(P_STATUS, line);
    if (!change.error.empty())
        ui_.append_line(P_STATUS, std::string("checkpoint ") + verb + ": " + change.error);
    ui_.draw();
}

agent::CheckpointService CheckpointCommands::service() const {
    const auto& window = ui_.win();
    const std::string root =
        window.root.empty()
            ? (window.project_root.empty() ? agent::Workspace::project_root() : window.project_root)
            : window.root;
    return {root, window.owner_id};
}

void CheckpointCommands::register_actions(SlashDispatcher& dispatcher) {
    dispatcher.register_action("core.session.history", [this](const std::string&) { history(); });
    dispatcher.register_action("core.session.undo", [this](const std::string&) { undo(); });
    dispatcher.register_action("core.session.rollback",
                               [this](const std::string& arg) { rollback(arg); });
    dispatcher.register_action("core.session.diff", [this](const std::string& arg) { diff(arg); });
}

void CheckpointCommands::history() {
    if (!require_chat())
        return;
    const auto service = this->service();
    std::string error;
    const auto checkpoints = service.history(error);
    if (!error.empty()) {
        ui_.append_line(P_STATUS, "checkpoints: " + error);
        ui_.draw();
        return;
    }
    if (checkpoints.empty()) {
        ui_.append_line(P_STATUS,
                        "checkpoints: none for this window (a file-changing turn creates one)");
        ui_.draw();
        return;
    }
    ui_.append_line(P_STATUS, "checkpoints (newest first, ref " + service.store().ref() + "):");
    for (const auto& row : agent::checkpoint_rows(checkpoints))
        ui_.append_line(P_STATUS, "  " + row);
    ui_.draw();
}

void CheckpointCommands::diff(const std::string& arg) {
    if (!require_chat())
        return;
    const auto service = this->service();
    std::string error;
    std::string text = service.diff(arg, error);
    if (!error.empty()) {
        ui_.append_line(P_STATUS, "checkpoint diff: " + error);
        ui_.draw();
        return;
    }
    if (text.empty()) {
        ui_.append_line(P_STATUS, "checkpoint diff: no changes");
        ui_.draw();
        return;
    }
    if (text.size() > kMaxDiff) {
        text.resize(kMaxDiff);
        text += "\n[diff truncated]";
    }
    ui_.append_line(P_STATUS, "checkpoint diff " + (arg.empty() ? "latest" : arg) + ":");
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        ui_.append_line(P_STATUS, line);
    ui_.draw();
}

void CheckpointCommands::undo() {
    if (!require_chat())
        return;
    if (ui_.busy_reject("undo"))
        return;
    auto service = this->service();
    std::string error;
    const auto checkpoints = service.history(error);
    if (!error.empty()) {
        ui_.append_line(P_STATUS, "checkpoint undo: " + error);
        ui_.draw();
        return;
    }
    if (checkpoints.size() < 2) {
        ui_.append_line(P_STATUS, "checkpoint undo: no earlier checkpoint");
        ui_.draw();
        return;
    }
    const auto& target = checkpoints[1];
    ConfirmPanel confirm("Undo checkpoint", "Restore the working tree to " +
                                                agent::checkpoint_describe(target) +
                                                "? A new checkpoint records the revert.");
    if (!confirm.run())
        return;
    const auto change = service.undo();
    if (change.kind == agent::CheckpointChangeKind::Failed) {
        ui_.append_line(P_STATUS, "checkpoint undo: " + change.error);
        ui_.draw();
        return;
    }
    report_restore(change, "undo");
}

void CheckpointCommands::rollback(const std::string& arg) {
    if (!require_chat())
        return;
    if (arg.empty()) {
        ui_.append_line(P_STATUS, "usage: /rollback <turn|sha>");
        ui_.draw();
        return;
    }
    if (ui_.busy_reject("rollback"))
        return;
    auto service = this->service();
    std::string error;
    const auto target = service.store().find(arg, error);
    if (!error.empty()) {
        ui_.append_line(P_STATUS, "checkpoint rollback: " + error);
        ui_.draw();
        return;
    }
    if (!target) {
        ui_.append_line(P_STATUS, "checkpoint rollback: no checkpoint for '" + arg + "'");
        ui_.draw();
        return;
    }
    ConfirmPanel confirm("Rollback checkpoint", "Restore the working tree to " +
                                                    agent::checkpoint_describe(*target) +
                                                    "? A new checkpoint records the revert.");
    if (!confirm.run())
        return;
    const auto change = service.rollback(arg);
    if (change.kind == agent::CheckpointChangeKind::Failed) {
        ui_.append_line(P_STATUS, "checkpoint rollback: " + change.error);
        ui_.draw();
        return;
    }
    report_restore(change, "rollback");
}

} // namespace tui
