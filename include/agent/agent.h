// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <fstream>
#include <set>
#include "agent/config.h"
#include "agent/registry.h"
#include "agent/llm.h"
#include "agent/conversation_log.h"
#include "agent/compressor.h"
#include "agent/experience.h"

namespace agent {

// Coarse activity state for a status-bar connection indicator.
enum class RunState : std::uint8_t {
    Idle,        // waiting, no request in flight
    Waiting,     // request sent, awaiting first byte
    Thinking,    // reasoning tokens arriving
    Streaming,   // answer tokens arriving
    Tooling,     // executing a tool call
    Error        // last request failed
};

// The host's answer when the agent asks permission to run an approval-gated
// tool (e.g. the shell tool). AllowSession grants the tool for the rest of the
// conversation; the harness, not the library, remembers that grant.
enum class Approval : std::uint8_t {
    Deny,           // reject this invocation
    AllowOnce,      // permit just this one call
    AllowSession    // permit this and future calls of the same tool this session
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

    // Consulted before running a tool whose requires_approval() is true. Given
    // the tool name and a human-readable summary of the action, returns the
    // host's decision. If unset, approval-gated tools are DENIED by default
    // (fail-safe: a headless run never executes shell commands unattended).
    std::function<Approval(const std::string& tool, const json& args,
                           const std::string& summary)> on_approval;

    // Emitted for every internal step (tool calls, tool results, state
    // transitions, errors) when the host has debug tracing enabled. Lets a UI
    // mirror the agent's internals to the screen without the core knowing
    // about rendering. The default no-op is used by headless runs.
    std::function<void(const std::string&)> on_debug;
};

// The core agent loop. Given an initial user prompt it drives the conversation:
//   1. send messages + tool schemas to the LLM
//   2. if the model emits tool_calls, execute them via the registry
//   3. feed results back and repeat until the model replies with plain text
//      or max_tool_iterations is reached.
class Agent {
public:
    Agent(const Config& cfg, ToolRegistry& registry, AgentHooks hooks = {},
          std::unique_ptr<CompressionStrategy> compressor = {},
          std::unique_ptr<CompressionGate> gate = {},
          std::unique_ptr<MemoryStore> memory_store = {},
          std::unique_ptr<MemoryRetriever> retriever = {});

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

    // Force immediate compression of the conversation history, bypassing
    // the gate.  The compressed history replaces the full history; this is
    // a one-way operation.  Returns a summary of what was saved.
    CompressionResult compress_now();

    // Internal metadata — never sent to the LLM.  Persisted alongside
    // history_ in the session file for future internal use.
    json meta_ = json::object();

    // Result of the most recent compression, or default-constructed if none.
    const CompressionResult& last_compression_result() const {
        return last_compression_;
    }

    // Number of memories and skills extracted during the last async pass.
    struct ExtractionResult {
        size_t new_memories = 0;
        size_t new_skills = 0;
    };
    ExtractionResult last_extraction_result() const {
        return last_extraction_;
    }

    // Replace the UI callbacks. Lets a long-lived agent receive fresh closures
    // each turn (e.g. a TUI window rebinding lambdas that capture live state).
    void set_hooks(AgentHooks hooks) { hooks_ = std::move(hooks); }

    // Enable or disable raw HTTP debug logging at runtime, pointing the log at
    // `path` (empty disables). Lets the TUI /debug toggle drive the client's
    // file tracing without reconstructing the agent.
    void set_debug_log(const std::string& path) {
        cfg_.debug_log = path;
        client_.set_debug_log(path);
    }

    // Enable or disable detection subsystems at runtime (/set detection namespace).
    void set_detection_loop(bool on) { cfg_.detection_loop = on; }
    void set_detection_duplicate(bool on) { cfg_.detection_duplicate = on; }

private:
    // Build and push the system message if the conversation is empty. Idempotent.
    void ensure_system_prompt();

    // One model round-trip (stream or buffered). When `display` is false the
    // token/reasoning/assistant hooks are suppressed so the exchange (e.g. the
    // internal confirmation check) never paints into the scrollback.
    Message chat_once(const std::vector<Tool*>& tools, bool display = true);

    // Hooks with the display callbacks removed, for silent internal exchanges.
    const AgentHooks& silent_hooks() const;

    // Ask the model to confirm/finish; dispatch any further tool calls it
    // requests, or return the accepted final text. Empty return means "keep
    // iterating the main loop".
    std::string confirm_turn(const std::string& candidate,
                             const std::vector<Tool*>& tools);

    Config cfg_;
    ToolRegistry& registry_;
    LLMClient client_;
    AgentHooks hooks_;
    ConversationLog log_;
    std::vector<Message> history_;
    std::set<std::string> session_approved_;  // tools granted for the session
    std::unique_ptr<CompressionStrategy> compression_;
    std::unique_ptr<CompressionGate> gate_;
    std::unique_ptr<MemoryStore> memory_store_;
    std::unique_ptr<MemoryRetriever> retriever_;
    ExperienceConfig experience_cfg_;
    size_t turn_counter_ = 0;
    CompressionResult last_compression_;
    ExtractionResult last_extraction_;
};

} // namespace agent

#endif // AGENT_AGENT_H
