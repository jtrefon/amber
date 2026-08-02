
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
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/policy.h"
#include "agent/skill_catalog.h"
#include "agent/tool_recovery.h"

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
// conversation; AlwaysAllow/AlwaysDeny persist to the policy store.
enum class Approval : std::uint8_t {
    Deny,           // reject this invocation only
    AllowOnce,      // permit just this one call (no state change)
    AllowSession,   // permit for the rest of this conversation
    AlwaysAllow,    // persist — never ask again for this tool
    AlwaysDeny      // persist — always block this tool
};

// Builds an LLM client for a given Config. The default factory wires the real
// HttpLLMClient; tests inject a factory returning a fake so runtime config
// changes (Agent::set_model) stay observable and network-free.
using LLMClientFactory =
    std::function<std::unique_ptr<LLMClient>(const Config&)>;

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
    Agent(Config cfg, ToolRegistry& registry, AgentHooks hooks = {},
          std::unique_ptr<CompressionStrategy> compressor = {},
          std::unique_ptr<CompressionGate> gate = {},
          std::unique_ptr<MemoryStore> memory_store = {},
          std::unique_ptr<MemoryRetriever> retriever = {},
          std::unique_ptr<LLMClient> client = {},
          LLMClientFactory client_factory = {});

    // Run one turn to completion, appending to the ongoing conversation.
    // Context from previous turns is retained (the agent is stateful). Returns
    // the final assistant reply text.
    std::string run(const std::string& user_prompt);

    // Read-only access to the conversation context stack.
    // Used by UIs to persist a session.
    const Context& context() const { return context_; }

    // Replace the conversation with a previously saved one (e.g. loaded from a
    // session file). Clears telemetry log association.
    void set_context(std::vector<Message> messages);

    // Force immediate compression of the conversation history, bypassing
    // the gate.  The compressed history replaces the full history (via
    // stack pop-all + push); this is a one-way operation.  Returns a
    // summary of what was saved.
    CompressionResult compress_now(std::function<void()> progress_cb = {});

    // Check whether the compression gate would fire on the current context
    // (without actually running compression). Useful for triggering
    // compression on session load.
    bool should_compress();

    // Internal metadata — never sent to the LLM.  Persisted alongside
    // history_ in the session file for future internal use.
    json meta_ = json::object();

    // Replace the UI callbacks. Lets a long-lived agent receive fresh closures
    // each turn (e.g. a TUI window rebinding lambdas that capture live state).
    void set_hooks(AgentHooks hooks) { hooks_ = std::move(hooks); }

    // Enable or disable detection subsystems at runtime (/set detection namespace).
    void set_detection_loop(bool on) { cfg_.detection_loop = on; }
    void set_detection_duplicate(bool on) { cfg_.detection_duplicate = on; }

    // Switch the active model at runtime (/set model). LLM clients hold a
    // Config snapshot from construction, so the client is rebuilt through the
    // injected factory — the next turn talks to the new model.
    void set_model(const std::string& model);

    void set_compression_threshold(double t) {
        cfg_.compression_threshold = t;
        cfg_.compression_threshold_explicit = true;
        if (gate_) gate_->set_threshold(t);
    }
    void set_compression_min_turns(int n) {
        cfg_.compression_min_turns = n;
        cfg_.compression_min_turns_explicit = true;
        if (gate_) gate_->set_min_turns(n);
    }

    // Policy store for tool approval rules.
    PolicyStore& policy() { return policy_; }
    const PolicyStore& policy() const { return policy_; }

    // Runtime skill catalog (union view + overrides + body cache).
    SkillCatalog& skills() { return *skills_; }
    const SkillCatalog& skills() const { return *skills_; }

    // Subscribe to context change events (token count + message count).
    // Fires on every push/pop/clear.
    ContextEventSource& context_events() { return context_events_; }

    // The session's experience store (nullptr when experience is disabled).
    // Read-only use by the UI; mutation goes through learn_forget/learn_pin
    // so persistence stays in the core.
    MemoryStore* memory_store() { return memory_store_.get(); }
    const MemoryStore* memory_store() const { return memory_store_.get(); }

    // The resolved experience configuration (store path, budgets).
    const ExperienceConfig& experience_config() const { return experience_cfg_; }

    // Remove / pin one learned item, persisting to the experience store.
    // Returns "" on success or a typed error string.
    std::string learn_forget(const std::string& id);
    std::string learn_pin(const std::string& id, bool pinned);

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

    // Log the user event and push the user message to history.
    void log_and_push_user_prompt(const std::string& prompt);

    // Dispatch tool calls with loop detection. Returns true if the loop
    // should continue (tool calls were present and handled). Sets final_reply
    // on hard stop.
    bool dispatch_with_loop_detection(const json& tool_calls,
                                      const std::string& content,
                                      FailStreak& fail_streak,
                                      int& loop_count,
                                      std::string& last_loop_key,
                                      int& tool_recovery_attempts,
                                      std::string& final_reply);

    // Detect repeated text replies. Returns true if a hard text loop was
    // detected and final_reply was set.
    bool detect_text_loop(const std::string& content, int& text_loop_count,
                          std::string& last_text, std::string& final_reply);

    // Run confirm_turn and either break with the final reply or continue.
    std::string try_confirm(const std::string& candidate,
                            const std::vector<Tool*>& tools);

    // Build the tools vector and the chat lambda for the run loop.
    std::vector<Tool*> resolve_tools();

    // One LLM round-trip with graceful recovery for request-side 4xx: the
    // retry loop repairs the request once (drop tools on template-parser
    // rejection, swap to a server-known model on name rejection). `strict`
    // makes internal exchanges rethrow instead of faking a reply.
    Message chat_with_recovery(const std::vector<Tool*>& tools,
                               const char* stage, bool display, bool strict);

    // Push a generated reply into context, log, and fire context event.
    void push_reply(Message reply);

    // Finalize a turn: fallback on empty, log, update state.
    std::string finish_turn(std::string final_reply);

    // Apply memory/skill ops from a compression response to the store.
    // Calls apply_memory_ops, apply_skill_ops, decay_all, then save.
    // Updates last_extraction_ for UI reporting.
    void apply_compression_result(const CompressionResponse& cr);

    Config cfg_;
    ToolRegistry& registry_;
    LLMClientFactory client_factory_;
    std::unique_ptr<LLMClient> client_;
    AgentHooks hooks_;
    ConversationLog log_;
    Context context_;
    ContextEventSource context_events_;
    std::set<std::string> session_approved_;  // tools granted for the session
    std::unique_ptr<CompressionStrategy> compression_;
    std::unique_ptr<CompressionGate> gate_;
    std::unique_ptr<MemoryStore> memory_store_;
    std::unique_ptr<MemoryRetriever> retriever_;
    std::unique_ptr<SkillCatalog> skills_;
    ExperienceConfig experience_cfg_;
    PolicyStore policy_;
    size_t turn_counter_ = 0;
    CompressionResult last_compression_;
};

} // namespace agent

#endif // AGENT_AGENT_H
