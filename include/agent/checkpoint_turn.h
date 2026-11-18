#ifndef AGENT_CHECKPOINT_TURN_H
#define AGENT_CHECKPOINT_TURN_H

#include "agent/checkpoint_service.h"

#include <functional>
#include <string>

namespace agent {

// The host's turn boundary. Before the turn it captures the tree the turn
// starts from (including edits made between turns); after the turn it captures
// what the turn did. The report sink sees a line only when a commit was written
// or a git call failed, so a read-only turn stays silent.
class CheckpointTurn {
public:
    using Report = std::function<void(const std::string&)>;

    CheckpointTurn(std::string project_root, std::string session_id, Report report);

    void before();
    void after(const std::string& prompt);

private:
    void guarded(const std::function<CheckpointChange()>& snapshot);
    void report_change(const CheckpointChange& change);

    CheckpointService service_;
    Report report_;
};

} // namespace agent

#endif // AGENT_CHECKPOINT_TURN_H
