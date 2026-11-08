#ifndef AGENT_TASK_JOIN_H
#define AGENT_TASK_JOIN_H

#include "agent/task.h"

#include <chrono>
#include <string>
#include <vector>

namespace agent {

// How long a one-shot host waits for its background work before returning.
// `wake` is the caller's policy (run one follow-up turn or not); the join
// helper only waits, so the same helper serves the CLI and the bench.
struct JoinPolicy {
    std::chrono::milliseconds deadline{0};
    bool wake = false;
};

struct JoinResult {
    // Terminal by the time the wait ended, newest first.
    std::vector<TaskRecord> completed;
    // Still Queued/Running when the deadline (or cancellation) fired.
    std::vector<std::string> still_running;
};

// Wait for the tasks owned by `owner` that were live when the call started.
// The wait re-reads the inventory every 50 ms; a zero deadline only samples
// the current state. Cancellation ends the wait the same way a deadline does,
// and the result reports what remains.
JoinResult join_tasks(TaskService& tasks, const std::string& owner, JoinPolicy policy,
                      const CancellationToken& token);

} // namespace agent

#endif // AGENT_TASK_JOIN_H
