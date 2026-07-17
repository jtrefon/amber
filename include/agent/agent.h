// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include <vector>
#include <functional>
#include <string>
#include <fstream>
#include "agent/config.h"
#include "agent/registry.h"
#include "agent/llm.h"

namespace agent {

// Appends conversation events to a JSON Lines file for telemetry/audit. Each
// call writes one self-contained JSON object with a unix-millisecond timestamp
// and a session id. No-op (and cheap) when logging is disabled.
class ConversationLog {
public:
    // path may contain "{ts}", replaced with the session start timestamp.
    void open(const std::string& path);
    bool enabled() const { return out_.is_open(); }
    const std::string& session() const { return session_; }

    // event: "session_start", "user", "assistant", "reasoning",
    //        "tool_call", "tool_result", "error", "session_end".
    void event(const std::string& type, const json& fields);

private:
    std::ofstream out_;
    std::string session_;
};

// Coarse activity state for a status-bar connection indicator.
enum class RunState {
    Idle,        // waiting, no request in flight
    Waiting,     // request sent, awaiting first byte
    Thinking,    // reasoning tokens arriving
    Streaming,   // answer tokens arriving
    Tooling,     // executing a tool call
    Error        // last request failed
};

// A hook invoked on each significant event so UIs can render progress without
// the library knowing about them. The default no-op is used by headless runs.
struct AgentHooks {
    std::function<void(const std::string&)> on_assistant;   // final text msg
    std::function<void(const std::string&)> on_token;       // streamed text delta
    std::function<void(const std::string&)> on_reasoning;   // streamed thinking delta
    std::function<void(const std::string&, const json&)> on_tool_call;
    std::function<void(const std::string&, const ToolResult&)> on_tool_result;
    std::function<void(const std::string&)> on_status;
    std::function<void(RunState)> on_state;                 // activity transitions
    std::function<void(const Stats&)> on_stats;             // per-request telemetry
};

// The core agent loop. Given an initial user prompt it drives the conversation:
//   1. send messages + tool schemas to the LLM
//   2. if the model emits tool_calls, execute them via the registry
//   3. feed results back and repeat until the model replies with plain text
//      or max_tool_iterations is reached.
class Agent {
public:
    Agent(const Config& cfg, ToolRegistry& registry, AgentHooks hooks = {});

    // Run one turn to completion, appending to the ongoing conversation.
    // Context from previous turns is retained (the agent is stateful). Returns
    // the final assistant reply text.
    std::string run(const std::string& user_prompt);

    // Full conversation so far, including the system prompt (index 0 once
    // seeded). Used by UIs to persist a session.
    const std::vector<Message>& history() const { return history_; }

    // Replace the conversation with a previously saved one (e.g. loaded from a
    // session file). If `messages` has no leading system message, the system
    // prompt is (re)seeded on the next run. Clears telemetry log association.
    void set_history(std::vector<Message> messages);

    // Forget everything and start a fresh conversation on the next run.
    void reset();

    // Replace the UI callbacks. Lets a long-lived agent receive fresh closures
    // each turn (e.g. a TUI window rebinding lambdas that capture live state).
    void set_hooks(AgentHooks hooks) { hooks_ = std::move(hooks); }

private:
    // Build and push the system message if the conversation is empty. Idempotent.
    void ensure_system_prompt();

    Config cfg_;
    ToolRegistry& registry_;
    LLMClient client_;
    AgentHooks hooks_;
    ConversationLog log_;
    std::vector<Message> history_;
};

} // namespace agent

#endif // AGENT_AGENT_H
