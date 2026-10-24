
#ifndef AGENT_SUBAGENT_H
#define AGENT_SUBAGENT_H

// Host-owned executor for the task tool. Runs focused sub-agents with their
// own context and a custom system prompt (base system + worker directive).
// Execution mode is runtime-configurable:
//   parallel=false (serial): sub-agents run one at a time — sequential
//       requests share the system-prompt prefix, keeping provider prompt
//       caches warm (cache-sensitive providers like DeepSeek discount
//       cached hits ~90%; parallel requests with distinct prefixes pay
//       full price each).
//   parallel=true: sub-agents run concurrently (the dispatch already
//       executes parallel tool calls), capped by max().

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "agent/agent.h"

namespace agent {

// A tool-level progress tail (never per-token): newest lines survive, capped
// so a chatty sub-agent cannot grow its viewer without limit.
inline constexpr std::size_t kTaskTailMaxLines = 200;
inline constexpr std::size_t kTaskTailMaxBytes = 4096;
void append_task_tail(std::string& tail, const std::string& line);

// Point-in-time view of one task run. The executor owns the records; this is a
// copy so a viewer can render without holding the executor lock.
struct SubAgentTaskInfo {
    std::string id;
    std::string owner_id;
    std::string prompt_head;
    std::string state; // queued | running | done | failed | cancelled
    std::string current_tool;
    std::string result;
    std::string tail;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    bool usage_measured = false;
    long elapsed_s = 0;
    int slot = -1;
};

class SubAgentExecutor {
public:
    SubAgentExecutor() = default;

    void set_parallel(bool on) noexcept { parallel_.store(on); }
    bool parallel() const noexcept { return parallel_.load(); }
    void set_max(int n) noexcept { max_.store(n < 1 ? 1 : n); }
    int max() const noexcept { return max_.load(); }
    // How long a task waits for a slot or a serial sibling before reporting
    // back. Tests set this small; the default is generous.
    void set_slot_wait_ms(int ms) noexcept { slot_wait_ms_.store(ms < 1 ? 1 : ms); }
    int slot_wait_ms() const noexcept { return slot_wait_ms_.load(); }
    void set_max_iterations(int n) noexcept { max_iterations_.store(n < 1 ? 1 : n); }
    int max_iterations() const noexcept { return max_iterations_.load(); }

    int launched() const noexcept { return launched_.load(); }
    int active() const noexcept;
    int waiting() const noexcept { return waiting_.load(); }
    std::vector<SubAgentTaskInfo> tasks() const;

    void set_config(const Config& cfg) { cfg_ = cfg; }
    void set_hooks(const AgentHooks& hooks) { hooks_ = hooks; }
    // Host sink for sub-agent usage (called on the worker thread). The host
    // tags it with UsageSource::Subagent; the executor also records the totals
    // on the task itself.
    void set_usage_sink(std::function<void(const UsageSample&)> sink) {
        usage_sink_ = std::move(sink);
    }
    void set_factory(LLMClientFactory f) { factory_ = std::move(f); }

    // Run one focused sub-task; returns the sub-agent's final reply.
    // `err` is set on hard failure (cap exceeded, transport, nesting).
    std::string run_task(const std::string& prompt, ToolRegistry& reg, std::string& err);
    AgentHooks sub_hooks_of() const;
    void record_usage(const std::string& task_id, const Stats& stats);
    Config sub_config() const;

private:
    bool acquire_slot();
    // Serial-mode gate (true when parallel, or when the bounded wait succeeds).
    bool enter_serial_mode(std::unique_lock<std::timed_mutex>& guard, std::string& err);
    // Construct and run the sub-agent, containing any exception into `err`.
    std::string run_sub_agent(const std::string& prompt, ToolRegistry& reg, const Config& sub_cfg,
                              const AgentHooks& sub_hooks, std::string& err);
    void release_slot() noexcept;
    std::string add_task(const std::string& prompt);
    void update_task(const std::string& id, const std::string& state,
                     const std::string& current_tool = {}, const std::string& result = {});
    void append_tail(const std::string& id, const std::string& line);
    // The child's progress hooks: state, per-task usage and a tool-level tail;
    // never per-token events.
    AgentHooks progress_hooks(const std::string& task_id);
    void fail_task(const std::string& id, const std::string& error);
    struct TaskRecord {
        std::string id;
        std::string owner_id;
        std::string prompt_head;
        std::string state;
        std::string current_tool;
        std::string result;
        std::string tail;
        long prompt_tokens = 0;
        long completion_tokens = 0;
        bool usage_measured = false;
        int slot = -1;
        std::chrono::steady_clock::time_point started;
    };

    std::atomic<bool> parallel_{true};
    std::atomic<int> max_{4};
    std::atomic<int> max_iterations_{20};
    // How long a task waits for a slot, or for a serial-mode sibling, before giving up
    // and reporting back. Generous enough for a real queue, bounded enough that a wedged
    // sibling cannot hold the parent indefinitely. Moved here from a file-local constant
    // that the configurable timeout left behind as a fossil.
    std::atomic<int> slot_wait_ms_{120 * 1000}; // 2 min; see set_slot_wait_ms
    std::atomic<int> launched_{0};
    std::atomic<int> waiting_{0};
    std::atomic<long> next_task_{0};
    mutable std::mutex tasks_mutex_;
    std::vector<TaskRecord> tasks_;
    Config cfg_;
    AgentHooks hooks_;
    std::function<void(const UsageSample&)> usage_sink_;
    LLMClientFactory factory_;
    std::timed_mutex serial_mutex_;
    mutable std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
    int active_ = 0;
};

// Whether the current thread is executing inside a sub-agent (nesting guard
// for the task tool). Async dispatch workers inherit the dispatching thread's
// state via set_subagent_inherited so the guard survives thread hops.
// The report contract and role boundary a sub-agent's system prompt carries:
// the same assembly as the main agent's, plus this. Rendered by
// render_system_prompt() so a later prompt rebuild cannot lose it.
std::string worker_directive();

bool in_subagent() noexcept;
void set_subagent_inherited(bool value) noexcept;

} // namespace agent

#endif // AGENT_SUBAGENT_H
