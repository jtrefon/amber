#ifndef AGENT_TASK_H
#define AGENT_TASK_H

#include "agent/process.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agent {

class EventBus;
class TaskStore;

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
    // Last evidence of life (start or the newest progress line). A crash turns
    // Running into Unknown with this as ended_ms; a clean exit overwrites it.
    std::int64_t last_activity_ms = 0;
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

// Capture the running tool call's owner, project and worktree for a new record.
// Outside a host run scope the owner is "default" and the project is the
// process workspace, so a bare test still has one usable inventory.
TaskOwner task_owner_from_scope();

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

    // Create a record for an already-running backend and remember its id (a
    // JobService job, a watch record, a worker handle). The backend-specific
    // tool uses backend_id() to read output or map a cancel.
    std::string adopt_job(const std::string& job_id, TaskRecord record,
                          std::string* error = nullptr);
    // Name the backend of a record that was created before its backend id was
    // known (process_start creates the record first so a fast exit cannot race
    // the completion sink). False for an unknown or already-terminal record.
    bool bind_backend(const std::string& id, const std::string& backend_id);
    std::string backend_id(const std::string& id) const;

    // Block until the task is terminal, the deadline expires, or the caller
    // cancels. Returns nullopt on timeout/cancel. Never polls: transitions
    // notify the condition variable.
    std::optional<TaskRecord> wait_for_terminal(const std::string& id, int timeout_ms,
                                                const CancellationToken& token) const;

    std::optional<TaskRecord> get(const std::string& id) const;
    std::vector<TaskRecord> list(const std::string& owner_id) const;
    std::vector<TaskRecord> all() const;
    std::uint64_t generation() const;
    TaskLimits limits() const;

    // Durable records. Attach during host construction; a hermetic host (bench,
    // a probe) attaches none, and every save becomes a no-op. A transition is
    // saved before its event is published, so a crash after the event can lose
    // the message but never the outcome.
    void attach_store(TaskStore* store) noexcept { store_ = store; }
    // Load the store into the inventory and apply the restart rules: Running
    // becomes Unknown (ended at its last activity), Queued becomes Cancelled.
    // Returns how many records were restored.
    std::size_t restore_from_store();
    // A clean exit: Running becomes Interrupted, Queued becomes Cancelled, and
    // each change is saved. A crash skips this and leaves Unknown on restore.
    void shutdown();
    // Terminal records whose completion nobody has accepted yet, newest first.
    // The inbox seeds these on startup; delivery is idempotent by task id.
    std::vector<TaskRecord> undelivered() const;
    // The owner accepted this completion. Saves delivered_at_ms and returns
    // false when the record is missing or was already delivered.
    bool mark_delivered(const std::string& id, std::int64_t at_ms = 0);

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
    // The newest snapshot of `id`, saved under store_mtx_. Never called while
    // mtx_ is held: save() is I/O and must not block a reader or a transition.
    void persist(const std::string& id);

    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    std::map<std::string, Entry> tasks_;
    // A cancel owns the terminal transition while its backend handler runs. A
    // completion that races the handler is a duplicate, not a second opinion,
    // so finish() ignores it and cancel() records the confirmed outcome.
    std::set<std::string> cancelling_;
    // task id -> backend handle (job id, watch id, worker handle).
    std::map<std::string, std::string> backends_;
    EventBus* events_ = nullptr;
    TaskStore* store_ = nullptr;
    // Serializes saves. Each save re-reads the record under mtx_, so the last
    // save to run always writes the newest state, even when two workers finish
    // a transition out of order.
    mutable std::mutex store_mtx_;
    TaskLimits limits_;
    std::uint64_t generation_ = 0;
};

} // namespace agent

#endif // AGENT_TASK_H
