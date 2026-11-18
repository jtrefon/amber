#include "agent/checkpoint_turn.h"

#include <exception>
#include <string>
#include <utility>

namespace agent {

namespace {

std::string short_id(const std::string& id) {
    return id.substr(0, 8);
}

} // namespace

CheckpointTurn::CheckpointTurn(std::string project_root, std::string session_id, Report report)
    : service_(std::move(project_root), std::move(session_id)), report_(std::move(report)) {}

void CheckpointTurn::before() {
    guarded([this] { return service_.before_turn(); });
}

void CheckpointTurn::after(const std::string& prompt) {
    guarded([this, &prompt] { return service_.after_turn(prompt); });
}

void CheckpointTurn::guarded(const std::function<CheckpointChange()>& snapshot) {
    try {
        report_change(snapshot());
    } catch (const std::exception& e) {
        report_(std::string("checkpoint: ") + e.what());
    }
}

void CheckpointTurn::report_change(const CheckpointChange& change) {
    if (change.kind == CheckpointChangeKind::Failed) {
        report_("checkpoint: " + change.error);
        return;
    }
    if (change.kind != CheckpointChangeKind::Created)
        return;

    const std::string sha = short_id(change.checkpoint.id);
    const std::string files = std::to_string(change.checkpoint.files) + " files";
    if (change.checkpoint.phase == CheckpointPhase::Pre) {
        report_("checkpoint: " + change.checkpoint.prompt + " " + sha + " (" + files + ")");
        return;
    }
    report_("checkpoint: turn " + std::to_string(change.checkpoint.turn) + " " + sha + " (" +
            files + ")");
}

} // namespace agent
