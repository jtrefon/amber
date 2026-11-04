#ifndef AGENT_TASK_H
#define AGENT_TASK_H

#include "agent/process.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

class EventBus;

// One identity for every unit of background work. The execution engines
// (JobService, WatchService, SubAgentExecutor) remain backends; this is the
// only inventory and the only state machine. Contract:
// docs/spec/agent-loop/background-tasks.md.
enum class TaskKind : std::uint8_t { Command, Watch, Worker };

enum class TaskState : std::uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
    Interrupted,
    Unknown,
};

enum class TaskNotify : std::uint8_t { Auto, Digest, Silent };

struct TaskOwner {
    std::string owner_id;     // stable window/session identity
    std::string project_root; // captured at spawn
    std::string worktree;     // captured at spawn; empty == project root
};

struct TaskResult {
    int exit_code = -1;
    int signal = 0;
    std::string summary;       // bounded, model-facing
    std::string output_tail;   // bounded, e.g. 8 KiB
    std::string artifact_path; // full output/transcript, on demand
};

struct TaskRecord {
    std::string id;
    TaskKind kind = TaskKind::Command;
    TaskState state = TaskState::Queued;
    TaskNotify notify = TaskNotify::Auto;
    std::string label;
    TaskOwner owner;
    std::string command;
    std::string goal;
    std::string cwd;
    std::int64_t created_ms = 0;
    std::int64_t started_ms = 0;
    std::int64_t ended_ms = 0;
    std::string progress;
    TaskResult result;
    std::string error;
    std::string cancel_reason;
    std::int64_t delivered_at_ms = 0;
};

// Injected at construction; v1 has no config keys for these.
struct TaskLimits {
    int command_per_owner = 20;
    int command_global = 64;
    int watch_per_owner = 20;
    int watch_global = 64;
    int worker_per_owner = 1;
    int worker_global = 4;
    int total_per_owner = 32;
    int total_global = 96;
};

bool task_state_terminal(TaskState state) noexcept;
const char* task_state_name(TaskState state) noexcept;
const char* task_kind_name(TaskKind kind) noexcept;

// The registry. It owns records and transitions; it never executes work.
// Thread safety: one mutex for the map, event publication and backend calls
// outside it, a generation counter for pull-based consumers.
class TaskService {
public:
    explicit TaskService(EventBus& events, TaskLimits limits = {});

    // Insert a Queued record. On a cap violation returns an empty id and, when
    // `error` is non-null, a message ending in "do not retry".
    std::string create(TaskRecord record, std::string* error = nullptr);

    // Queued -> Running. `now_ms` is the wall clock at start.
    bool started(const std::string& id, std::int64_t started_at);

    // Replace the one-line progress field. Ignored after a terminal state.
    bool progress(const std::string& id, std::string line);

    // Running -> a terminal state. `Cancelled` must go through cancel(); this
    // rejects it so a backend cannot claim a cancel it did not perform.
    bool finish(const std::string& id, TaskState state, TaskResult result, std::string error = {});

    // Ask the backend to stop. The handler runs outside the map lock; success
    // moves to Cancelled, failure to Unknown with the reason recorded.
    bool cancel(const std::string& id, std::string reason, const CancellationToken& token);

    // The backend's cancellation entry point. A task without one cannot be
    // cancelled; cancel() then records Unknown rather than lying.
    bool attach_cancel(const std::string& id, std::function<bool(const CancellationToken&)> fn);

    std::optional<TaskRecord> get(const std::string& id) const;
    std::vector<TaskRecord> list(const std::string& owner_id) const;
    std::vector<TaskRecord> all() const;
    std::uint64_t generation() const;
    TaskLimits limits() const;

private:
    struct Entry {
        TaskRecord record;
        std::function<bool(const CancellationToken&)> cancel;
    };

    bool legal_transition(TaskState from, TaskState to) const noexcept;
    std::pair<int, int> caps_for(TaskKind kind) const noexcept;
    bool over_limit_locked(const TaskRecord& record) const noexcept;
    // The second half of cancel(), after the backend call, so the lock is not
    // held across it and the function stays inside the size cap.
    bool finish_cancel(const std::string& id, bool stopped, const std::string& reason);
    // Caller holds mtx_. Returns the event to publish after unlocking.
    enum class EventKind { None, Started, Progress, Completed, Cancelled };
    EventKind transition_locked(Entry& entry, TaskState to, EventKind event) noexcept;

    mutable std::mutex mtx_;
    std::map<std::string, Entry> tasks_;
    EventBus* events_ = nullptr;
    TaskLimits limits_;
    std::uint64_t generation_ = 0;
};

} // namespace agent

#endif // AGENT_TASK_H
