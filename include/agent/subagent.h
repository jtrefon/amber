#ifndef AGENT_SUBAGENT_H
#define AGENT_SUBAGENT_H

// Host-owned executor for the delegate tool. A delegated worker runs focused
// sub-agents with their own context and a custom system prompt (base system +
// worker directive). The worker is detached: spawn() creates a TaskService
// record and returns its id, and a pool of executor threads runs the child.
// Execution mode is runtime-configurable:
//   parallel=false (serial): one worker at a time — sequential requests share
//       the system-prompt prefix, keeping provider prompt caches warm
//       (cache-sensitive providers like DeepSeek discount cached hits ~90%;
//       parallel requests with distinct prefixes pay full price each).
//   parallel=true: workers run concurrently, capped by max().

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/task.h"

namespace agent {

class EventBus;
class TaskStore;

// A tool-level progress tail (never per-token): newest lines survive, capped
// so a chatty sub-agent cannot grow its viewer without limit.
inline constexpr std::size_t kTaskTailMaxLines = 200;
inline constexpr std::size_t kTaskTailMaxBytes = 4096;
void append_task_tail(std::string& tail, const std::string& line);

// What a caller asks a worker to do. goal/context/expect are required: the
// child cannot see the parent's conversation, so an empty context or an
// implicit success criterion is a malformed request, not a short prompt.
struct SpawnRequest {
    std::string goal;
    std::string context;
    std::string expect;
    std::string label;
    int max_turns = 0;
    TaskOwner owner;
};

class SubAgentExecutor {
public:
    SubAgentExecutor();
    ~SubAgentExecutor();

    SubAgentExecutor(const SubAgentExecutor&) = delete;
    SubAgentExecutor& operator=(const SubAgentExecutor&) = delete;

    void set_parallel(bool on) noexcept { parallel_.store(on); }
    bool parallel() const noexcept { return parallel_.load(); }
    void set_max(int n) noexcept { max_.store(n < 1 ? 1 : n); }
    int max() const noexcept { return max_.load(); }
    void set_max_iterations(int n) noexcept { max_iterations_.store(n < 1 ? 1 : n); }
    int max_iterations() const noexcept { return max_iterations_.load(); }

    int launched() const noexcept { return launched_.load(); }
    int active() const noexcept;
    int waiting() const noexcept;

    // The worker rows in the one inventory. Reimplemented over TaskService:
    // SubAgentTaskInfo is gone, so a host has a single record type.
    std::vector<TaskRecord> tasks() const;

    void set_config(const Config& cfg) { cfg_ = cfg; }
    void set_hooks(const AgentHooks& hooks) { hooks_ = hooks; }
    // Per-owner hooks for delegated workers. A run carries the owner id of the
    // turn that spawned it, so an approval reaches that window rather than
    // whichever window happens to be active. set_hooks() stays the single
    // owner fallback (headless runs and tests); an empty owner or no factory
    // uses it.
    using HooksFor = std::function<AgentHooks(const std::string& owner_id)>;
    void set_hooks_for(HooksFor f) { hooks_for_ = std::move(f); }
    // Host sink for sub-agent usage (called on the worker thread). The host
    // tags it with UsageSource::Subagent; the executor also records the totals
    // on the task record.
    void set_usage_sink(std::function<void(const UsageSample&)> sink) {
        usage_sink_ = std::move(sink);
    }
    void set_factory(LLMClientFactory f) { factory_ = std::move(f); }

    // The one inventory. A host owns the TaskService and wires it before any
    // spawn. Without one the executor owns a private service so a bare test or
    // a probe still has a working queue; a host never takes that path.
    void set_tasks(TaskService* tasks) noexcept;
    TaskService* task_service() noexcept;
    // Derives <root>/<task_id>/transcript.jsonl. Empty means no transcript is
    // written (the record still carries the report).
    void set_artifact_root(std::string root) { artifact_root_ = std::move(root); }

    // Create the task record, enqueue the work, and return the task id. The
    // call never waits for a slot or for the child. Validation failures and an
    // exhausted admission cap return an empty id with `err` set.
    std::string spawn(SpawnRequest request, ToolRegistry& reg, std::string& err);

    // Synchronous convenience for tests and non-host callers: spawn and wait
    // for the terminal record. The delegate tool itself uses spawn().
    std::string run_task(const std::string& prompt, ToolRegistry& reg, std::string& err);

    // Stop accepting work, cancel what is queued or running, and join every
    // executor thread. Idempotent; the destructor calls it.
    void shutdown();

    AgentHooks sub_hooks_of() const;
    AgentHooks sub_hooks_of(const std::string& owner_id) const;
    void record_usage(const std::string& task_id, const Stats& stats);
    Config sub_config() const;

private:
    struct Work {
        std::string task_id;
        SpawnRequest request;
        ToolRegistry* registry = nullptr;
    };

    void ensure_tasks();
    TaskOwner resolve_owner(const SpawnRequest& request) const;
    std::string create_worker_record(const SpawnRequest& request, const TaskOwner& owner,
                                     std::string& err);
    void enqueue_work(const std::string& task_id, SpawnRequest request, ToolRegistry& reg);
    void ensure_workers();
    void worker_loop();
    void run_work(const Work& work);
    bool begin_work(const Work& work, const CancellationToken& token);
    std::string execute_child(const Work& work, const CancellationToken& token, std::string& err,
                              std::string* transcript);
    void finish_work(const Work& work, const CancellationToken& token, const std::string& reply,
                     const std::string& err, std::string transcript);
    void cleanup_work(const std::string& task_id);
    // Requests the child's token and returns true once the worker is no longer
    // running it (or immediately when it is still queued).
    bool cancel_work(const std::string& task_id);
    // Construct and run the sub-agent, containing any exception into `err`.
    // `transcript` receives a JSONL rendering of the child's context.
    std::string run_sub_agent(ToolRegistry& reg, const Config& sub_cfg, const AgentHooks& sub_hooks,
                              const SpawnRequest& request, std::string& err,
                              std::string* transcript);
    // The child's progress hooks: state, per-task usage and a tool-level tail;
    // never per-token events.
    AgentHooks progress_hooks(const std::string& task_id);
    void append_tail(const std::string& task_id, const std::string& line);
    std::string take_tail(const std::string& task_id);
    std::string transcript_path(const std::string& task_id) const;

    std::atomic<bool> parallel_{true};
    std::atomic<int> max_{4};
    std::atomic<int> max_iterations_{20};
    std::atomic<int> launched_{0};

    mutable std::mutex tails_mutex_;
    std::map<std::string, std::string> tails_;
    std::vector<std::thread> workers_;
    std::deque<Work> queue_;
    std::set<std::string> running_;
    std::map<std::string, CancellationToken> tokens_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable idle_cv_;
    bool shutting_down_ = false;
    int worker_count_ = 0;

    Config cfg_;
    AgentHooks hooks_;
    HooksFor hooks_for_;
    std::function<void(const UsageSample&)> usage_sink_;
    LLMClientFactory factory_;
    TaskService* tasks_ = nullptr;
    std::unique_ptr<EventBus> internal_bus_;
    std::unique_ptr<TaskService> internal_tasks_;
    std::string artifact_root_;
};

// Whether the current thread is executing inside a sub-agent (nesting guard
// for the delegate tool). Async dispatch workers inherit the dispatching
// thread's state via set_subagent_inherited so the guard survives thread hops.
// The report contract and role boundary a sub-agent's system prompt carries:
// the same assembly as the main agent's, plus this. Rendered by
// render_system_prompt() so a later prompt rebuild cannot lose it.
std::string worker_directive();

bool in_subagent() noexcept;
void set_subagent_inherited(bool value) noexcept;

} // namespace agent

#endif // AGENT_SUBAGENT_H
