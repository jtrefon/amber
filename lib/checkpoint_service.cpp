#include "agent/checkpoint_service.h"

#include <string>
#include <utility>

namespace agent {

namespace {

CheckpointChange failed(std::string error) {
    CheckpointChange change;
    change.kind = CheckpointChangeKind::Failed;
    change.error = std::move(error);
    return change;
}

} // namespace

CheckpointService::CheckpointService(std::string project_root, std::string session_id)
    : store_(std::move(project_root), std::move(session_id)) {}

CheckpointChange CheckpointService::record(const std::string& prompt, std::size_t turn,
                                           CheckpointPhase phase) {
    CheckpointChange change;
    std::string error;
    const auto created = store_.create(turn, prompt, error, phase);
    if (created) {
        change.kind = CheckpointChangeKind::Created;
        change.checkpoint = *created;
    } else if (!error.empty()) {
        change.kind = CheckpointChangeKind::Failed;
        change.error = std::move(error);
    }
    return change;
}

CheckpointChange CheckpointService::before_turn() {
    std::string error;
    const auto list = store_.list(error);
    if (!error.empty())
        return failed(std::move(error));

    const std::size_t turn = list.empty() ? 0 : list.front().turn;
    const std::string prompt =
        list.empty() ? "initial state" : "before turn " + std::to_string(turn + 1);
    return record(prompt, turn, CheckpointPhase::Pre);
}

CheckpointChange CheckpointService::after_turn(const std::string& prompt) {
    std::string error;
    const auto list = store_.list(error);
    if (!error.empty())
        return failed(std::move(error));

    const std::size_t turn = list.empty() ? 1 : list.front().turn + 1;
    return record(prompt, turn, CheckpointPhase::Post);
}

std::vector<Checkpoint> CheckpointService::history(std::string& error) const {
    return store_.list(error);
}

std::string CheckpointService::diff(const std::string& selector, std::string& error) const {
    if (selector.empty()) {
        const auto list = store_.list(error);
        if (!error.empty())
            return {};
        if (list.empty()) {
            error = "no checkpoints yet";
            return {};
        }
        return store_.diff(list.front(), error);
    }
    const auto target = store_.find(selector, error);
    if (!error.empty())
        return {};
    if (!target) {
        error = "no checkpoint for '" + selector + "'";
        return {};
    }
    return store_.diff(*target, error);
}

CheckpointChange CheckpointService::undo() {
    std::string error;
    const auto list = store_.list(error);
    if (!error.empty())
        return failed(std::move(error));
    if (list.size() < 2)
        return failed("no earlier checkpoint to undo to");

    // The checkpoint before the tip is the state the last turn started from:
    // a pre-turn snapshot when the host wrote one, otherwise the previous
    // post-turn checkpoint.
    return restore_to(list[1], "undo to turn " + std::to_string(list[1].turn));
}

CheckpointChange CheckpointService::rollback(const std::string& selector) {
    std::string error;
    const auto target = store_.find(selector, error);
    if (!error.empty())
        return failed(std::move(error));
    if (!target)
        return failed("no checkpoint for '" + selector + "'");
    return restore_to(*target, "rollback to turn " + std::to_string(target->turn));
}

CheckpointChange CheckpointService::restore_to(const Checkpoint& target,
                                               const std::string& prompt) {
    CheckpointChange change;
    change.kind = CheckpointChangeKind::Restored;
    change.target = target;

    std::string error;
    if (!store_.restore(target, error)) {
        change.kind = CheckpointChangeKind::Failed;
        change.error = std::move(error);
        return change;
    }

    const auto list = store_.list(error);
    if (!error.empty()) {
        change.error = "restored, but could not read the history: " + error;
        return change;
    }
    const std::size_t turn = list.empty() ? 1 : list.front().turn + 1;
    const auto created = store_.create(turn, prompt, error, CheckpointPhase::Post);
    if (created)
        change.checkpoint = *created;
    else if (!error.empty())
        change.error = "restored, but could not record the revert: " + error;
    return change;
}

} // namespace agent
