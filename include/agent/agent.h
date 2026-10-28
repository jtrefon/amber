
#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "agent/config.h"
#include "agent/registry.h"
#include "agent/llm.h"
#include "agent/events.h"
#include "agent/extensions.h"
#include "agent/conversation_log.h"
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/policy.h"
#include "agent/prompt_priorities.h"
#include "agent/session_brief.h"
#include "agent/skill_catalog.h"
#include "agent/activation_sink.h"
#include "agent/repair.h"
#include "agent/tool_recovery.h"
#include "agent/turn.h"

namespace agent {

class ShapeQueue;

// Coarse activity state for a status-bar connection indicator.
enum class RunState : std::uint8_t {
    Idle,      // waiting, no request in flight
    Waiting,   // request sent, awaiting first byte
    Thinking,  // reasoning tokens arriving
    Streaming, // answer tokens arriving
    Tooling,   // executing a tool call
    Error      // last request failed
};

// The host's answer when the agent asks permission to run an approval-gated
// tool (e.g. the shell tool). AllowSession grants the tool for the rest of the
// conversation; AlwaysAllow/AlwaysDeny persist to the policy store.
enum class Approval : std::uint8_t {
    Deny,         // reject this invocation only
    AllowOnce,    // permit just this one call (no state change)
    AllowSession, // permit for the rest of this conversation
    AlwaysAllow,  // persist — never ask again for this tool
    AlwaysDeny    // persist — always block this tool
};

// Builds an LLM client for a given Config. The default factory wires the real
// HttpLLMClient; tests inject a factory returning a fake so runtime config
// changes (Agent::set_model) stay observable and network-free.
using LLMClientFactory = std::function<std::unique_ptr<LLMClient>(const Config&)>;

// A hook invoked on each significant event so UIs can render progress without
// the library knowing about them. The default no-op is used by headless runs.
// Where a request's usage came from: the chat itself, a sub-agent it spawned,
// a compression pass, or a watch observer. Hosts keep them apart so a session's
// tree total can be attributed.
enum class UsageSource { Main, Subagent, Compression, Watch };

struct UsageSample {
    UsageSource source = UsageSource::Main;
    long prompt_tokens = -1;
    long completion_tokens = -1;
    std::string task_id; // sub-agent samples only
    std::string owner_id;
};

struct AgentHooks {
    std::function<void(const std::string&)> on_assistant; // final text msg
    std::function<void(const std::string&)> on_token;     // streamed text delta
    std::function<void(const std::string&)> on_reasoning; // streamed thinking delta
    std::function<void(const std::string&, const json&)> on_tool_call;
    // (name, result, args) — args are the original tool-call arguments so
    // hosts can describe what was actually run.
    std::function<void(const std::string&, const ToolResult&, const json&)> on_tool_result;
    std::function<void(const std::string&)> on_status;
    std::function<void(RunState)> on_state;     // activity transitions
    std::function<void(const Stats&)> on_stats; // per-request telemetry
    // Per-request usage with a source tag. on_stats stays the raw telemetry
    // stream; this is what a session ledger accumulates.
    std::function<void(const UsageSample&)> on_usage;

    // One completed context-compaction attempt, success or failure. Hosts use
    // it for the session ledger and the benchmark KPI; the prose status lines
    // stay for humans.
    std::function<void(const CompressionResult&)> on_compression;

    // Consulted before running a tool whose requires_approval() is true. Given
    // the tool name and a human-readable summary of the action, returns the
    // host's decision. If unset, approval-gated tools are DENIED by default
    // (fail-safe: a headless run never executes shell commands unattended).
    std::function<Approval(const std::string& tool, const json& args, const std::string& summary)>
        on_approval;

    // Ask the host for an API key/token for the active provider. `reason` is
    // human-readable ("provider 'kilocode' requires an API key", "API key
    // rejected (HTTP 401)"). The host is responsible for persisting the key to
    // the provider's config; the core only rebuilds the client and retries.
    // Returns the entered key, or an empty string when the user
    // declines/cancels or no host prompt exists (fail-safe: headless runs
    // never block on input).
    std::function<std::string(const std::string& reason)> on_api_key;

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
    // A prompt block waiting to be placed, ordered by (priority, insertion).
    struct PromptBlock {
        int priority = 0;
        std::size_t seq = 0;
        std::string text;
    };

    Agent(Config cfg, ToolRegistry& registry, AgentHooks hooks = {},
          std::unique_ptr<CompressionStrategy> compressor = {},
          std::unique_ptr<CompressionGate> gate = {},
          std::shared_ptr<MemoryStore> memory_store = {},
          std::unique_ptr<MemoryRetriever> retriever = {}, std::unique_ptr<LLMClient> client = {},
          LLMClientFactory client_factory = {}, std::shared_ptr<SkillCatalog> skills = {});
    // `memory_store`/`skills` are shared_ptr because a project scopes them:
    // every agent on one workspace shares ONE catalog and ONE store. The
    // catalog is the RUNNING agent's — the skill tools resolve it through
    // RunScope (run_scope.h), so whichever agent constructs them, a call is
    // always answered by the agent that is running. Hosts with several agents
    // on one project (TUI windows) inject the shared instances; single-agent
    // hosts pass nothing and get private ones. The skill tools themselves are
    // installed by the tool_skills plugin, not here.

    // Run one turn to completion, appending to the ongoing conversation.
    // Context from previous turns is retained (the agent is stateful). Returns
    // the final assistant reply text.
    std::string run(const std::string& user_prompt);

    // The turn's diagnostics sink. The host records the failures that escape
    // run() — they never reach a recovery path inside the agent — through the
    // same journal the agent writes.
    ConversationLog& log() noexcept { return log_; }

    // Read-only access to the conversation context stack.
    // Used by UIs to persist a session.
    const Context& context() const { return context_; }

    // Replace the conversation with a previously saved one (e.g. loaded from a
    // session file). Clears telemetry log association.
    void set_context(std::vector<Message> messages);

    // The host's shaping queue: the model's plan waits here until the next
    // request boundary. Null means the host did not wire shaping.
    void set_shape_queue(ShapeQueue* queue) { shape_queue_ = queue; }
    ShapeQueue* shape_queue() const { return shape_queue_; }

    // Force immediate compression of the conversation history, bypassing
    // the gate.  The compressed history replaces the full history (via
    // stack pop-all + push); this is a one-way operation.  Returns a
    // summary of what was saved.
    CompressionResult compress_now(std::function<void()> progress_cb = {});

    // Run the compression pipeline (gate already decided). Rebuilds the
    // context only on success — a failed pipeline leaves the context
    // untouched (spec invariant 7). Optionally fills `out` with stats.
    bool run_compression(std::function<void()> progress_cb, CompressionResult* out,
                         const CompressionRequestPrefix* prefix = nullptr);

    // Internal metadata — never sent to the LLM.  Persisted alongside
    // history_ in the session file for future internal use.
    json meta_ = json::object();

    // Replace the UI callbacks. Lets a long-lived agent receive fresh closures
    // each turn (e.g. a TUI window rebinding lambdas that capture live state).
    void set_hooks(AgentHooks hooks) { hooks_ = std::move(hooks); }

    // Enable or disable detection subsystems at runtime (/set detection namespace).
    void set_detection_loop(bool on) { cfg_.detection_loop = on; }
    void set_detection_duplicate(bool on) { cfg_.detection_duplicate = on; }

    // The project this agent belongs to. Empty means the process default.
    // Installed into every run scope so state, instructions and tools resolve
    // against the window's project rather than the one amber was started in.
    void set_project_root(std::string root);

    // Confine this agent's tools to `root` (a window's worktree). Empty means
    // the project root. Installed into every run scope, so tools running on
    // dispatch workers confine to the same tree as the agent.
    void set_workspace_root(std::string root);

    // The host's stable identity for this agent's window/session. It travels
    // in the run scope so a tool that starts work outliving the turn can route
    // its completion back to the right window.
    void set_owner_id(std::string id) { owner_id_ = std::move(id); }

    // Switch the active model at runtime (/set model). LLM clients hold a
    // Config snapshot from construction, so the client is rebuilt through the
    // injected factory — the next turn talks to the new model. `window` is
    // the new model's probed context window (0 = unknown). An automatic
    // resolution never becomes a user pin: when the model changes and the
    // window is unknown the previous model's value is cleared rather than
    // reused. A user pin (context_explicit) survives.
    void set_model(const std::string& model, const ModelCapabilities& caps = {});

    // Apply the active model's advertised capabilities after a catalogue
    // refresh. Rebuilds the client (it holds a Config snapshot) and publishes.
    void set_model_capabilities(const ModelCapabilities& caps);

    // Apply a probed window (or a provider default) to the current model
    // without rebuilding the client. Used when a catalogue arrives after the
    // model was chosen, and by the host after a provider switch. Ignored when
    // the user pinned context_size explicitly.
    void set_context_window(int window);
    // A deliberate override: /set model context. The pin survives model and
    // provider switches and is persisted by the host. clear_context_pin drops
    // it and re-resolves from the current model's known window or the learned
    // overflow clamp; unknown stays 0.
    void pin_context_window(int window);
    void clear_context_pin();
    void set_reasoning_effort(const std::string& effort);
    void set_connection(const std::string& api_base, const std::string& api_key,
                        const std::string& model);

    // Follow the host's provider selection (/set provider, and /set model when
    // the picked row belongs to another provider).
    //
    // Identity, endpoint, key and dialect land together, from the host's
    // already-resolved Config: the status bar reads provider_name from the
    // agent's snapshot, the next request goes to api_base with api_key, and the
    // rebuilt client speaks flavor. A switch that moves only some of them shows
    // the old owner beside the new model -- and routes that model to the old
    // endpoint, which is a kilo model requested from openrouter.
    void set_provider(const Config& resolved, const std::string& model);

    void set_compression_threshold(double t) {
        cfg_.compression_threshold = t;
        cfg_.compression_threshold_explicit = true;
        if (gate_)
            gate_->set_threshold(t);
    }
    void set_compression_min_turns(int n) {
        cfg_.compression_min_turns = n;
        cfg_.compression_min_turns_explicit = true;
        if (gate_)
            gate_->set_min_turns(n);
    }
    void set_compression_target_pct(int pct) {
        cfg_.compression_target_pct = pct;
        cfg_.compression_target_pct_explicit = true;
    }
    void set_compression_keep_last_prompts(int n) {
        cfg_.compression_keep_last_prompts = n;
        cfg_.compression_keep_last_prompts_explicit = true;
    }

    // Immutable snapshot of the resolved runtime config (model, mode, thinking)
    // — per-agent, so a window's status bar and session snapshot describe THIS
    // agent, not the host template.
    //
    // The agent's worker owns `cfg_` outright (single ownership, not locking), so
    // readers on other threads — the UI renders from this every frame — must take
    // the snapshot instead of a reference. Every display-relevant mutator
    // publishes a fresh copy, so a reader can never observe a half-written
    // std::string. Internal counters (turn_counter, prompt_tokens_used) are
    // deliberately not published: nothing outside the worker reads them.
    std::shared_ptr<const Config> config_snapshot() const {
        return std::atomic_load_explicit(&published_cfg_, std::memory_order_acquire);
    }

    // Ask the running turn to stop (the loop polls between iterations and
    // cancellable tools consult it through RunScope). Each agent owns its
    // token — cancelling one window never touches a sibling's run.
    void request_cancel() { cfg_.cancel_token.request(); }

    // Rebind this agent to be an independent copy of `src`: identical
    // conversation context, runtime config, approvals, model windows, turn
    // counter, brief store, and session-activated skills — so both legs
    // serialize the same wire prefix (KV-cache reuse) and then diverge
    // cleanly. The fork keeps its own cancel token, hooks, log, and event
    // wiring. Caller guarantees `src` is quiescent (Context is
    // single-owner; never fork mid-run).
    void fork_from(const Agent& src);

    // Switch the operational mode at runtime (/mode). Mode feeds tool
    // availability and approval policy, both read from cfg_ per turn, so
    // the next run picks it up — and the system prompt renders from cfg_,
    // which makes a mode change a new wire prefix for the next request.
    // Publishes the config snapshot so the status bar reflects it immediately.
    void set_mode(AgentMode mode);
    void set_thinking(const std::string& thinking) { cfg_.thinking = thinking; }

    // Session-activated skill bodies (own list — see inject_prompt_blocks).
    // Snapshot, not a reference: the sink is written from tool workers.
    std::vector<ActivatedSkill> activated_skills() const { return activated_skills_.snapshot(); }

    // Policy store for tool approval rules.
    PolicyStore& policy() { return policy_; }
    const PolicyStore& policy() const { return policy_; }

    // Runtime skill catalog (union view + overrides + body cache).
    SkillCatalog& skills() { return *skills_; }
    const SkillCatalog& skills() const { return *skills_; }

    // Subscribe to context change events (token count + message count).
    // Fires on every push/pop/clear.
    ContextEventSource& context_events() { return context_events_; }

    // Attach the harness event bus. Optional: an agent without one publishes
    // nothing, and every publish site is a single atomic load away from a
    // no-op, so a plugin subscription costs the loop nothing until it exists.
    // Events are observable; hidden internal exchanges (the confirmation
    // probe) are never published.
    void set_events(EventBus& bus) noexcept { event_bus_ = &bus; }

    // Attach the contribution registries. Optional, like the bus: with none
    // attached the agent builds its prompt from the core blocks alone.
    void set_prompt_registry(PromptRegistry& prompts) noexcept { prompt_registry_ = &prompts; }

    // The session's experience store (nullptr when experience is disabled).
    // Read-only use by the UI; mutation goes through learn_forget/learn_pin
    // so persistence stays in the core.
    MemoryStore* memory_store() { return memory_store_.get(); }
    const MemoryStore* memory_store() const { return memory_store_.get(); }

    // The session brief store (always present — the brief is harness-owned,
    // not model-owned, so it exists regardless of experience config).
    SessionBriefStore* session_brief_store() { return &brief_store_; }
    const SessionBriefStore* session_brief_store() const { return &brief_store_; }
    // Real user prompts this session. The compression cooldown is expressed in
    // these; LLM calls are counted separately.
    size_t user_turn_counter() const noexcept { return user_turn_counter_; }

    // The resolved experience configuration (store path, budgets).
    const ExperienceConfig& experience_config() const { return experience_cfg_; }

    // Remove / pin one learned item, persisting to the experience store.
    // Returns "" on success or a typed error string.
    std::string learn_forget(const std::string& id);
    std::string learn_pin(const std::string& id, bool pinned);

private:
    // The root this agent's tools are confined to, when it is not the project
    // root: a window working in its own worktree sets it at creation, and every
    // run installs it into the run scope so dispatch workers inherit it.
    std::string project_root_;
    std::string workspace_root_;
    std::string owner_id_;
    // Environment note for the prompt when that root is a detached worktree
    // (see worktree_session_note). Computed when the root is set, never per
    // turn: rendering the system prompt must not shell out.
    std::string workspace_note_;
    // Publish the display-relevant config for other threads. Copy-on-write:
    // `cfg_` stays the worker's private working copy, readers get an immutable
    // snapshot. Atomic so an idle-window UI write (set_model) can publish too.
    void publish_config();
    std::shared_ptr<const Config> published_cfg_; // atomic access only
    // Assemble every injected prompt block into `prompt_copy`, in one ordered
    // pass. Called exactly once per request and only AFTER the compression
    // gate: a block injected before the rebuild is discarded when the rebuild
    // replaces the prompt, which is the bug this replaced (retrieved memories
    // vanished on compressing turns). Head blocks are inserted with the system
    // prompt, tail blocks follow the conversation; ordering is
    // `prompt_priority`. The sealed Context is never touched — this is the
    // prompt copy.
    void inject_prompt_blocks(std::vector<Message>& prompt_copy) const;
    // The blocks that injection would add, in request order. Compression
    // replays them so its classify/extract calls are extensions of the cached
    // prefix (see CompressionRequestPrefix).
    void collect_prompt_blocks(const std::vector<Message>& prompt_copy,
                               std::vector<PromptBlock>& head,
                               std::vector<PromptBlock>& tail) const;
    CompressionRequestPrefix
    compression_prefix(const std::vector<Message>& history,
                       const std::vector<std::shared_ptr<Tool>>& tools) const;

    // Build and push the system message if the conversation is empty. Idempotent.
    void ensure_system_prompt();

    // One model round-trip (stream or buffered).
    Message chat_once(const std::vector<std::shared_ptr<Tool>>& tools);
    bool maybe_compress(std::vector<Message>& prompt_copy,
                        const std::vector<std::shared_ptr<Tool>>& tools);
    void on_stream_chunk(const AgentHooks& h, const StreamChunk& ch) const;
    void record_stats(const Stats& stats);
    void publish_llm_response(const Stats& stats);
    void decay_and_persist();
    std::function<std::function<Message()>(const std::string&)>
    build_chat_adapter(const char* stage, const std::vector<std::shared_ptr<Tool>>& tools);

    // Bridges the repair ladder to this agent's config, hooks and log. The
    // strategies themselves never see an Agent (repair.h).
    class RepairBridge;
    RepairLadder repairs_ = make_default_repair_ladder();
    std::string retrieve_memory_suffix(const std::vector<Message>& prompt_copy) const;
    void collect_skill_blocks(std::vector<PromptBlock>& head, std::vector<PromptBlock>& tail,
                              std::size_t& seq) const;

    void open_turn(const std::string& user_prompt);
    bool fail_compression(CompressionResult* out, const std::string& error, size_t msgs_before);
    void publish_compression_done(bool success, long tokens_after);
    bool fail_compression_after(CompressionResult& r, const std::string& error, size_t msgs_before,
                                size_t tokens_before, CompressionReporter& reporter,
                                CompressionResult* out);
    bool run_compression_pipeline(CompressionReporter& reporter, CompressionResponse& cr,
                                  std::vector<Message>& compressed,
                                  const CompressionRequestPrefix& prefix);
    void rebuild_context(std::vector<Message>& compressed);

    // Validate and apply plans the model submitted, at this request boundary.
    // Once per user turn, and only when the freed content is worth the rebuild a
    // changed prefix forces. Returns true when the context was rebuilt.
    bool apply_pending_shapes(std::vector<Message>& prompt_copy);
    void count_segments(const CompressionResponse& cr, CompressionResult& r);
    void finish_compression(CompressionResult& r, const CompressionResponse& cr, size_t msgs_before,
                            size_t tokens_before, CompressionReporter& reporter,
                            CompressionResult* out);
    bool turn_budget_exceeded(Turn& turn);
    bool advance_turn(Turn& turn, const std::vector<std::shared_ptr<Tool>>& tools, Message& reply);

    // advance_turn under a recovery guard: a failure raised while folding a
    // reply into the turn (dispatch, tool envelopes, host callbacks) is
    // reported and survived instead of escaping run() and killing the turn.
    bool fold_reply(Turn& turn, const std::vector<std::shared_ptr<Tool>>& tools, Message& reply);

    // Fold a non-chat failure into the turn: log it, tell the host, hand the
    // model a message it can act on, and answer any tool_call the failure
    // swallowed. Returns true when the loop must stop instead.
    bool recover_internal_failure(Turn& turn, const std::string& what);

    // Push a [harness] notice (retry summary, repair report, loop state, stop
    // reason) into the conversation. Harness notices ride the user channel with
    // Message::harness set: they reach the model in place, and the loop knows
    // they are not the model's own reply.
    void inject_harness_note(const std::string& text);

    // The loop's own state, said once at the moment it changes: how much of the
    // iteration and wall-clock budget is spent, and whether the same call keeps
    // coming back. Nothing here intervenes — the model is told and decides.
    void note_loop_state(Turn& turn, int iter);

    // The four ways one reply can fold into the turn. Each returns true when the
    // loop must stop.
    bool fold_tool_calls(Turn& turn, const json& tool_calls, const std::string& content);
    bool fold_harness_notice(Turn& turn);
    bool fold_silent_reply(Turn& turn);

    // A tool_call with no tool response is an invalid request and poisons every
    // later turn; a swallowed dispatch failure leaves one behind.
    void answer_orphan_tool_calls(const std::string& what);

    // Log the user event and push the user message to history.
    void log_and_push_user_prompt(const std::string& prompt);

    // Dispatch tool calls with loop detection. Returns true if the loop
    // should continue (tool calls were present and handled). Sets the turn's
    // stop reason on a hard stop.
    bool dispatch_with_loop_detection(const json& tool_calls, const std::string& content,
                                      Turn& turn);

    // The dispatch guards: the same call fingerprint repeated past the guard
    // ends the turn, and a failing tool earns one recovery steer before it
    // stops.
    bool stop_on_dispatch_loop(const json& tool_calls, Turn& turn);
    void fold_fail_streak(const json& tool_calls, bool dispatch_ok, Turn& turn);

    // Detect repeated text replies. Returns true if a hard text loop was
    // detected and the turn's stop reason was set.
    bool detect_text_loop(Turn& turn, const std::string& content);

    // Build the tools vector and the chat lambda for the run loop.
    std::vector<std::shared_ptr<Tool>> resolve_tools();

    // One LLM round-trip with graceful recovery for request-side 4xx: the
    // retry loop repairs the request once (drop tools on template-parser
    // rejection, swap to a server-known model on name rejection).
    Message chat_with_recovery(const std::vector<std::shared_ptr<Tool>>& tools, const char* stage);

    // Push a generated reply into context, log, and fire context event.
    void push_reply(Message reply);

    // Extract text-embedded XML tool calls (attribute or name/arguments
    // style) from a reply's content into the structured tool_calls field.
    // Clears content when calls were found, so template-style tool calls
    // execute like structured ones.
    bool extract_embedded_tool_calls(Message& reply) const;

    // Finalize a turn: fallback on empty, report the stop, log, update state.
    // `st.model_closed` false means a bound ended the turn (iteration cap, wall
    // clock, loop guard, silence) — its reason is recorded as a harness notice.
    std::string finish_turn(Turn& turn);
    std::string finish_turn_cancelled();

    // Apply memory/skill ops from a compression response to the store.
    // Calls apply_memory_ops, apply_skill_ops, decay_all, then save.
    // Updates last_extraction_ for UI reporting.
    void apply_compression_result(const CompressionResponse& cr);

    // Merge the extracted session brief into the brief store (called after
    // a successful compression cycle). Non-fatal: a missing brief is a no-op.
    void apply_brief(const CompressionResponse& cr);

    // Resolve the effective context window for the gate and gauge: the
    // active model's probed window (when the user did not set context_size
    // explicitly), clamped by any window the server taught us via a 400
    // overflow rejection. An unknown window stays 0 (no auto-compression).
    void resolve_window();

    // Publish a typed event when a bus is attached. No bus, or no subscriber
    // for this payload type, means the call compiles down to a null check.
    template <class E> void publish_event(E& event) {
        if (!event_bus_)
            return;
        Events(*event_bus_).publish(event);
    }

    // Announce a message that has just been sealed into the context.
    void publish_message_added(const Message& msg);
    // Announce a request failure the loop is about to repair or surface.
    void publish_error(const std::string& kind, const std::string& message, bool retryable = false);

    // Per-model probed windows (model id -> context), fed by /set model.
    std::map<std::string, int> model_windows_;

    Config cfg_;
    ToolRegistry& registry_;
    LLMClientFactory client_factory_;
    std::unique_ptr<LLMClient> client_;
    AgentHooks hooks_;
    ConversationLog log_;
    Context context_;
    ContextEventSource context_events_;
    std::set<std::string> session_approved_; // tools granted for the session
    ShapeQueue* shape_queue_ = nullptr;
    std::size_t last_shape_turn_ = 0;
    std::unique_ptr<CompressionStrategy> compression_;
    std::unique_ptr<CompressionGate> gate_;
    std::shared_ptr<MemoryStore> memory_store_;
    std::unique_ptr<MemoryRetriever> retriever_;
    std::shared_ptr<SkillCatalog> skills_;
    // Session-activated skill bodies, in activation order. Per-agent state
    // even though the catalog is shared: a skill read in this session is
    // injected into THIS session's prompt copy only.
    ActivationSink activated_skills_;
    SessionBriefStore brief_store_;
    ExperienceConfig experience_cfg_;
    PolicyStore policy_;
    EventBus* event_bus_ = nullptr;
    PromptRegistry* prompt_registry_ = nullptr;
    // The system prompt is derived, not stored: its inputs include the tool
    // registry and the plugin prompt blocks, and a toggle changes both.
    std::string render_system_prompt() const;
    void push_system_prompt(std::string system);
    // LLM calls this session (pacing, memory freshness).
    size_t turn_counter_ = 0;
    // Real user prompts this session: the compression cooldown's unit, because
    // one tool-using turn is many LLM calls.
    size_t user_turn_counter_ = 0;
    CompressionResult last_compression_;
};

} // namespace agent

#endif // AGENT_AGENT_H
