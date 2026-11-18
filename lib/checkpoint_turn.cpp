#include "agent/checkpoint_turn.h"

#include <exception>
#include <string>
#include <utility>

namespace agent {

namespace {

std::string short_id(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 8) : id;
}

} // namespace

CheckpointTurn::CheckpointTurn(std::string project_root, std::string session_id, Report report)
    : service_(std::move(project_root), std::move(session_id)), report_(std::move(report)) {}

void CheckpointTurn::before() {
    try {
        report_change(service_.before_turn());
    } catch (const std::exception& e) {
        if (report_)
            report_(std::string("checkpoint: ") + e.what());
    } catch (...) {
        if (report_)
            report_("checkpoint: unknown failure");
    }
}

void CheckpointTurn::after(const std::string& prompt) {
    try {
        report_change(service_.after_turn(prompt));
    } catch (const std::exception& e) {
        if (report_)
            report_(std::string("checkpoint: ") + e.what());
    } catch (...) {
        if (report_)
            report_("checkpoint: unknown failure");
    }
}

void CheckpointTurn::report_change(const CheckpointChange& change) {
    if (!report_)
        return;
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
