
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
#include <condition_variable>
#include <mutex>
#include <string>

#include "agent/agent.h"

namespace agent {

class SubAgentExecutor {
public:
    SubAgentExecutor() = default;

    void set_parallel(bool on) noexcept { parallel_.store(on); }
    bool parallel() const noexcept { return parallel_.load(); }
    void set_max(int n) noexcept { max_.store(n < 1 ? 1 : n); }
    int max() const noexcept { return max_.load(); }
    void set_max_iterations(int n) noexcept { max_iterations_.store(n < 1 ? 1 : n); }
    int max_iterations() const noexcept { return max_iterations_.load(); }

    int launched() const noexcept { return launched_.load(); }

    void set_config(const Config& cfg) { cfg_ = cfg; }
    void set_hooks(const AgentHooks& hooks) { hooks_ = hooks; }
    void set_factory(LLMClientFactory f) { factory_ = std::move(f); }

    // Run one focused sub-task; returns the sub-agent's final reply.
    // `err` is set on hard failure (cap exceeded, transport, nesting).
    std::string run_task(const std::string& prompt, ToolRegistry& reg,
                         std::string& err);

private:
    bool acquire_slot();
    void release_slot() noexcept;

    std::atomic<bool> parallel_{true};
    std::atomic<int> max_{4};
    std::atomic<int> max_iterations_{20};
    std::atomic<int> launched_{0};
    Config cfg_;
    AgentHooks hooks_;
    LLMClientFactory factory_;
    std::mutex serial_mutex_;
    std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
    int active_ = 0;
};

// Whether the current thread is executing inside a sub-agent (nesting guard
// for the task tool). Async dispatch workers inherit the dispatching thread's
// state via set_subagent_inherited so the guard survives thread hops.
bool in_subagent() noexcept;
void set_subagent_inherited(bool value) noexcept;

} // namespace agent

#endif // AGENT_SUBAGENT_H
