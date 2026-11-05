#ifndef AGENT_TASK_INBOX_H
#define AGENT_TASK_INBOX_H

#include "agent/events.h"
#include "agent/task.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace agent {

class EventBus;
class TaskService;

// One completion waiting for its owner's next safe boundary. `prompt_text` is
// one bounded line; the caller's digest adds the harness header.
struct TaskCompletion {
    std::string task_id;
    // The producer's own handle (a watch id, a job id); the inbox's producer
    // ack hook uses it, while task_id names the shared inventory record.
    std::string backend_id;
    std::string owner_id;
    TaskKind kind = TaskKind::Command;
    TaskState state = TaskState::Unknown;
    std::string prompt_text;
    std::string artifact_path;
    std::int64_t created_ms = 0;
};

// One bounded harness message for an owner's pending completions. Pure so the
// shape is testable without a queue; the caller may append a "still running"
// summary from TaskService.
std::string format_task_digest(const std::vector<TaskCompletion>& completions);

// The in-process courier between TaskService's completion event and the
// owning agent's next safe boundary. It never touches Context and never wakes a
// turn; the host decides when a boundary exists. `take` marks ids taken (not
// acked); `ack` after the agent accepted the digest makes delivery idempotent.
class TaskInbox {
public:
    TaskInbox(EventBus& events, TaskService& tasks);
    ~TaskInbox();

    TaskInbox(const TaskInbox&) = delete;
    TaskInbox& operator=(const TaskInbox&) = delete;

    // A durable producer (the watch plugin) installs this once: after the
    // inbox accepted a completion, the hook marks the producer's own record
    // delivered. It runs outside the inbox lock.
    using AckHook = std::function<void(const TaskCompletion&)>;
    void set_ack_hook(AckHook hook);

    void post(TaskCompletion completion);
    std::vector<TaskCompletion> take(const std::string& owner_id);
    void ack(const std::string& owner_id, const std::vector<std::string>& ids);
    // Drop everything an owner has pending (window close); the producer is
    // abandoned separately, so no hook runs.
    void drop(const std::string& owner_id);
    std::size_t pending(const std::string& owner_id) const;
    std::size_t size() const;

private:
    void on_completed(const TaskCompletedEvent& event);

    TaskService& tasks_;
    Subscription subscription_;
    mutable std::mutex mtx_;
    // owner -> task id -> completion. A map keeps the post path idempotent:
    // the same id replaces its line rather than queueing twice.
    std::map<std::string, std::map<std::string, TaskCompletion>> pending_;
    std::set<std::string> taken_;
    std::set<std::string> acked_;
    AckHook ack_hook_;
};

} // namespace agent

#endif // AGENT_TASK_INBOX_H
