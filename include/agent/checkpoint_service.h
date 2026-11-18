#ifndef AGENT_CHECKPOINT_SERVICE_H
#define AGENT_CHECKPOINT_SERVICE_H

#include "agent/checkpoint.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agent {

enum class CheckpointChangeKind : std::uint8_t {
    None,     // the tree was unchanged, so nothing was written
    Created,  // a checkpoint was written
    Restored, // the tree was put back to `target`
    Failed,   // the operation did not complete
};

struct CheckpointChange {
    CheckpointChangeKind kind = CheckpointChangeKind::None;
    // Created: the new snapshot. Restored: the revert snapshot, empty when the
    // restored tree was already the tip (nothing to record).
    Checkpoint checkpoint;
    // Restored: the snapshot the tree was put back to.
    Checkpoint target;
    // Failed, or a recording failure after a successful restore.
    std::string error;

    bool ok() const noexcept { return kind != CheckpointChangeKind::Failed; }
};

// The turn rules on top of CheckpointStore: which turn number a snapshot
// belongs to, when a pre-turn snapshot is worth writing, and the rule that a
// restore is itself a checkpoint so it can be undone.
class CheckpointService {
public:
    CheckpointService(std::string project_root, std::string session_id);

    // The state the next turn starts from. A tree changed since the last
    // checkpoint is recorded under that checkpoint's turn with a "before turn
    // N" prompt; an unchanged tree writes nothing.
    CheckpointChange before_turn();

    // The state after a turn. The next turn number is the tip's turn + 1.
    CheckpointChange after_turn(const std::string& prompt);

    std::vector<Checkpoint> history(std::string& error) const;
    std::string diff(const std::string& selector, std::string& error) const;

    // Put the tree back to the checkpoint before the newest one, then record
    // the revert as a new checkpoint. The second undo returns to the state
    // before the first.
    CheckpointChange undo();

    // Put the tree back to `selector` (a turn number or a SHA prefix), then
    // record the revert as a new checkpoint.
    CheckpointChange rollback(const std::string& selector);

    const CheckpointStore& store() const noexcept { return store_; }

private:
    CheckpointChange record(const std::string& prompt, std::size_t turn, CheckpointPhase phase);
    CheckpointChange restore_to(const Checkpoint& target, const std::string& prompt);

    CheckpointStore store_;
};

} // namespace agent

#endif // AGENT_CHECKPOINT_SERVICE_H
