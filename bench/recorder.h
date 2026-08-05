
#ifndef BENCH_RECORDER_H
#define BENCH_RECORDER_H

// AgentHooks observer that turns an Agent run into a structured event stream
// for KPI computation. Pairs tool calls with results, parses the status
// stream for retry/recovery/loop signals, accumulates per-request stats.
// The Recorder must outlive the Agent it observes.

#include <string>
#include <vector>

#include "agent/agent.h"
#include "bench/oracle.h"

namespace bench {

struct ToolEvent {
    std::string name;
    agent::json args;
    bool ok = false;
    std::string error;
    bool denied = false;
    bool timeout = false;
    long duration_ms = 0;
};

struct RetryEvent {
    int attempt = 0;
};

struct RecoveryEvent {
    std::string kind;   // "repaired" | "steer" | "model"
};

struct StatsEvent {
    double latency_ms = -1;
    double tps = -1;
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

struct EventStream {
    std::vector<ToolCallEvent> calls;   // in the agent's chosen order
    std::vector<ToolEvent> tools;
    std::vector<RetryEvent> retries;
    std::vector<RecoveryEvent> recoveries;
    std::vector<StatsEvent> stats;
    int compressions = 0;           // "compressing N messages..." status lines
    int iterations = 0;             // max "iteration N/M" observed
    double ttft_ms = -1;            // first request's time-to-first-byte
    long prompt_tokens = 0;
    long completion_tokens = 0;
    bool hard_stop = false;
};

class Recorder {
public:
    Recorder() = default;

    // Hooks to hand to Agent (captures this; Recorder must outlive the Agent).
    agent::AgentHooks hooks();

    const EventStream& stream() const noexcept { return stream_; }

    // Direct feeds for unit tests (also invoked by the hook lambdas).
    void on_tool_call(const std::string& name, const agent::json& args);
    void on_tool_result(const std::string& name, const agent::ToolResult& res);
    void on_status(const std::string& text);
    void on_debug(const std::string& text);
    void on_stats(const agent::Stats& s);
    void on_state(agent::RunState st);

private:
    struct Pending {
        std::string fingerprint;
        std::string name;
        agent::json args;
        long t0_ms;
    };

    static std::string fingerprint(const std::string& name,
                                   const agent::json& args) noexcept;
    static long now_ms() noexcept;

    EventStream stream_;
    std::vector<Pending> pending_;
};

// Parse one status line into the stream (retries, repairs, steers, hard stops).
void parse_status(const std::string& text, EventStream& out) noexcept;

// Parse one debug line (iteration counters) into the stream.
void parse_debug(const std::string& text, EventStream& out) noexcept;

} // namespace bench

#endif // BENCH_RECORDER_H
