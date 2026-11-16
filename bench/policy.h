#ifndef BENCH_POLICY_H
#define BENCH_POLICY_H

// A deterministic, closed-loop policy for hermetic benchmark runs. Unlike a
// fake_replies transcript, a policy sees the conversation and the last tool
// result before choosing the next step.

#include <memory>
#include <string>
#include <vector>

#include "agent/llm.h"
#include "agent/tool.h"

namespace bench {

struct PolicyContext {
    int step = 0;
    const std::vector<agent::Message>* messages = nullptr;
    const std::vector<std::shared_ptr<agent::Tool>>* tools = nullptr;
    std::string last_tool_name;
    std::string last_tool_status; // ok | error | denied | timeout | empty
    std::string last_tool_result;
};

struct PolicyReply {
    std::string content;
    agent::json tool_calls = agent::json::array();
    std::string reasoning;
    std::string error;
    bool retryable = true;
    long latency_ms = 0;
    int drop_after_chunks = 0;
    std::string chunk_delta = "x";
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

class Policy {
public:
    virtual ~Policy() = default;
    virtual PolicyReply next(const PolicyContext& context) = 0;
};

// Build a policy from a JSON state machine. Returns nullptr and fills `err` for
// an unknown state, an unreachable terminal, an unbounded cycle or a malformed
// action/condition.
std::unique_ptr<Policy> make_state_machine_policy(const agent::json& spec, std::string& err);

// True when `spec` is a valid state machine; `err` explains the first failure.
bool validate_state_machine(const agent::json& spec, std::string& err);

} // namespace bench

#endif // BENCH_POLICY_H
