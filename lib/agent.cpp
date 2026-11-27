
#include "agent/agent.h"

#include "agent/task_inbox.h"
#include "agent/config_paths.h"
#include "agent/environment.h"
#include "agent/json_read.h"
#include "agent/plugin.h"
#include "agent/prompt.h"
#include "agent/subagent.h"
#include "agent/tool_call_parser.h"
#include "agent/agent_helpers.h"
#include "agent/context_shape.h"
#include "agent/dispatch.h"
#include "agent/tools.h"
#include "agent/model_probe.h"
#include "agent/data_path.h"
#include "agent/workspace.h"
#include "agent/worktree.h"
#include "agent/run_scope.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent {

namespace {

// Helper: publish context-change event to all subscribers.
void emit_context_event(ContextEventSource& src, const Context& ctx) {
    src.publish(ctx.token_count(), ctx.size());
}

} // namespace

void Agent::publish_message_added(const Message& msg) {
    MessageAddedEvent added;
    added.message = &msg;
    added.index = context_.get_all().size() - 1;
    publish_event(added);
}

void Agent::publish_error(const std::string& kind, const std::string& message, bool retryable) {
    ErrorRaisedEvent raised;
    raised.kind = kind;
    raised.message = message;
    raised.retryable = retryable;
    publish_event(raised);
    // The event bus is plugin-facing. The host's status channel is what the user
    // reads, and a failure that exists only in the journal is a failure nobody
    // can act on (a 502 during compaction used to be invisible).
    if (hooks_.on_status)
        hooks_.on_status("error: " + kind + ": " + message);
}

namespace {

// Build an LLM client for `cfg` through the injected factory, falling back
// to the real HttpLLMClient when no factory was provided.
std::unique_ptr<LLMClient> make_client(const Config& cfg, const LLMClientFactory& factory) {
    return factory ? factory(cfg) : std::make_unique<HttpLLMClient>(cfg);
}

} // namespace

Agent::Agent(Config cfg, ToolRegistry& registry, AgentHooks hooks, AgentDeps deps)
    : cfg_(std::move(cfg)), registry_(registry), client_factory_(std::move(deps.client_factory)),
      client_(std::move(deps.client)), hooks_(std::move(hooks)),
      compression_controller_(std::make_unique<CompressionController>(std::move(deps.compressor),
                                                                      std::move(deps.gate))),
      memory_store_(std::move(deps.memory_store)), retriever_(std::move(deps.retriever)) {
    loop_guard_ =
        std::make_unique<LoopGuard>(cfg_, hooks_, log_, context_, context_events_,
                                    [this](const std::string& text) { inject_harness_note(text); });
    if (!client_)
        client_ = make_client(cfg_, client_factory_);
    // A window's workspace may be a detached worktree; the prompt says so before
    // the agent can commit to an unreachable HEAD. The main checkout has no
    // worktree note (the check is a file test, not a git call, in that case).
    workspace_note_ = worktree_session_note(Workspace::project_root());
    experience_cfg_ = load_experience_config(cfg_);
    if (deps.skills) {
        skills_ = std::move(deps.skills);
    } else {
        skills_ = std::make_shared<SkillCatalog>(cfg_);
        std::vector<Skill> learned;
        if (memory_store_)
            learned = memory_store_->top_skills(experience_cfg_.max_skills, "");
        skills_->discover(learned);
    }
    publish_config(); // readers must never see a null snapshot
}

void Agent::publish_config() {
    // Copy-on-write: the worker keeps mutating its private `cfg_`; readers on
    // other threads (the UI renders from this every frame) get an immutable
    // snapshot. Atomic, so an idle-window UI write publishes without a lock and
    // a reader can never observe a half-written std::string.
    std::atomic_store_explicit(&published_cfg_, std::make_shared<const Config>(cfg_),
                               std::memory_order_release);
}

void Agent::set_owner_id(std::string id) {
    owner_id_ = std::move(id);
    set_session_id(owner_id_);
}

void Agent::set_session_id(std::string id) {
    if (cfg_.session_id == id)
        return;
    cfg_.session_id = std::move(id);
    // The client owns a Config copy, so tell it directly rather than rebuilding
    // it (an injected client must survive).
    if (client_)
        client_->set_session_id(cfg_.session_id);
    publish_config();
}

void Agent::set_mode(AgentMode mode) {
    cfg_.mode = mode;
    // The status bar renders from the published snapshot, not cfg_; without
    // this the mode changed for the next turn but the bar stayed on the old
    // value (/set policy mode looked like it did nothing).
    publish_config();
}

void Agent::set_model(const std::string& model, const ModelCapabilities& caps) {
    const bool model_changed = model != cfg_.model;
    cfg_.model = model;
    cfg_.model_explicit = true;
    if (caps.context > 0) {
        model_windows_[model] = caps.context;
        cfg_.context_limit = caps.context;
    } else if (model_changed) {
        cfg_.context_limit = 0;
    }
    // A catalogue pick is an automatic resolution, not a user pin. Apply the
    // window it reports; when the new model reports none, clear the previous
    // model's value instead of pretending it still applies. A pin can lower
    // the listing's window but never exceed it.
    if (!cfg_.context_explicit) {
        if (caps.context > 0)
            cfg_.context_size = caps.context;
        else if (model_changed)
            cfg_.context_size = 0;
    } else if (caps.context > 0 && cfg_.context_size > caps.context) {
        cfg_.context_size = caps.context;
    }
    // Capabilities follow the model. A same-model call with no capabilities
    // keeps what the catalogue already established.
    if (model_changed || has_capabilities(caps))
        apply_model_capabilities(cfg_, caps);
    client_ = make_client(cfg_, client_factory_);
    publish_config();
}

void Agent::set_model_capabilities(const ModelCapabilities& caps) {
    const bool changed = cfg_.model_max_output != caps.max_output ||
                         cfg_.model_supported_parameters != caps.supported_parameters ||
                         cfg_.model_line != caps.line || cfg_.model_reasoning != caps.reasoning ||
                         (caps.context > 0 && cfg_.context_limit != caps.context);
    if (!changed)
        return;
    if (caps.context > 0) {
        cfg_.context_limit = caps.context;
        if (cfg_.context_size > caps.context)
            cfg_.context_size = caps.context;
    }
    apply_model_capabilities(cfg_, caps);
    client_ = make_client(cfg_, client_factory_);
    publish_config();
}

void Agent::set_context_window(int window) {
    if (cfg_.context_explicit)
        return;
    if (cfg_.context_limit > 0 && window > cfg_.context_limit)
        window = cfg_.context_limit;
    cfg_.context_size = window;
    if (window > 0)
        model_windows_[cfg_.model] = window;
    publish_config();
}

void Agent::set_context_limit(int limit) {
    if (limit <= 0)
        return;
    cfg_.context_limit = limit;
    if (cfg_.context_size > limit)
        cfg_.context_size = limit;
    publish_config();
}

void Agent::pin_context_window(int window) {
    if (cfg_.context_limit > 0 && window > cfg_.context_limit)
        window = cfg_.context_limit;
    cfg_.context_size = window;
    cfg_.context_explicit = true;
    publish_config();
}

void Agent::clear_context_pin() {
    cfg_.context_explicit = false;
    // resolve_window() only overwrites from a known map entry; clearing the
    // value first is what prevents the pin itself being left behind.
    cfg_.context_size = 0;
    resolve_window();
}

void Agent::resolve_window() {
    // Tier 1: the active model's probed window (unless the user pinned
    // context_size explicitly). Tier 2: anything the server taught us via
    // a 400 overflow rejection clamps the result — the rejection is the
    // runtime truth (a model's trained n_ctx may exceed the server's
    // actual --ctx-size). Unknown stays 0: the gate never guesses.
    if (!cfg_.context_explicit) {
        auto it = model_windows_.find(cfg_.model);
        if (it != model_windows_.end() && it->second > 0)
            cfg_.context_size = it->second;
    }
    if (client_) {
        const int learned = client_->learned_context_size();
        if (learned > 0 && (cfg_.context_size == 0 || learned < cfg_.context_size))
            cfg_.context_size = learned;
    }
    if (cfg_.context_limit > 0 && cfg_.context_size > cfg_.context_limit)
        cfg_.context_size = cfg_.context_limit;
    publish_config();
}

void Agent::set_reasoning_effort(const std::string& effort) {
    cfg_.reasoning_effort = effort;
    client_ = make_client(cfg_, client_factory_);
    publish_config();
}

void Agent::set_connection(const std::string& api_base, const std::string& api_key,
                           const std::string& model) {
    cfg_.api_base = api_base;
    if (!api_key.empty())
        cfg_.api_key = api_key;
    if (!model.empty()) {
        cfg_.model = model;
        cfg_.model_explicit = true;
    }
    client_ = make_client(cfg_, client_factory_);
    publish_config();
}

void Agent::set_provider(const Config& resolved, const std::string& model) {
    cfg_.provider_name = resolved.provider_name;
    cfg_.profile_name = resolved.profile_name;
    cfg_.api_base = resolved.api_base;
    cfg_.api_key = resolved.api_key;
    cfg_.flavor = resolved.flavor;
    // Sampling belongs to the connection: a profile carries it, and the
    // provider-switch callers pass the live values, so this is a no-op there.
    cfg_.temperature = resolved.temperature;
    cfg_.max_tokens = resolved.max_tokens;
    cfg_.thinking = resolved.thinking;
    // The window belongs to the endpoint/model pair that was just selected.
    // A user pin survives; an unpinned agent adopts the resolved default (0
    // when the provider has none) instead of keeping the previous provider's.
    cfg_.context_limit = resolved.context_limit;
    if (!cfg_.context_explicit) {
        cfg_.context_size = resolved.context_size;
    } else if (cfg_.context_limit > 0 && cfg_.context_size > cfg_.context_limit) {
        cfg_.context_size = cfg_.context_limit;
    }
    apply_model_capabilities(cfg_, capabilities_of(resolved));
    // The host resolved this model for the connection, so it is adopted even
    // when it is empty ("auto-detect"): a profile whose model is auto must not
    // keep sending the outgoing connection's model to the new endpoint.
    cfg_.model = model;
    cfg_.model_explicit = !model.empty();
    // A response id and a prompt count belong to the connection that produced
    // them; replaying either against a new one is a stale-cache request.
    cfg_.previous_response_id.clear();
    cfg_.prompt_tokens_used = -1;
    client_ = make_client(cfg_, client_factory_);
    publish_config();
}

void Agent::set_project_root(std::string root) {
    project_root_ = std::move(root);
    workspace_note_ = worktree_session_note(workspace_root_.empty() ? Workspace::project_root()
                                                                    : workspace_root_);
}

void Agent::set_workspace_root(std::string root) {
    workspace_root_ = std::move(root);
    workspace_note_ = worktree_session_note(workspace_root_.empty() ? Workspace::project_root()
                                                                    : workspace_root_);
}

std::string Agent::render_system_prompt() const {
    // Environment card: OS, user, resources, available tools — collected
    // once at session start, so the agent can act in its environment without
    // probing. Session-fixed by construction: re-probing every turn would both
    // shell out per request and rewrite the cached prefix when the answer moved.
    if (!environment_probed_) {
        environment_card_ = render_environment_card(probe_environment());
        environment_probed_ = true;
    }
    SystemPromptSources sources;
    sources.cfg = &cfg_;
    sources.registry = &registry_;
    sources.prompts = prompt_registry_;
    sources.environment_card = &environment_card_;
    sources.workspace_note = &workspace_note_;
    sources.subagent = in_subagent();
    return PromptAssembler::system_prompt(sources);
}

PromptSources Agent::prompt_sources() const {
    PromptSources sources;
    sources.read_skill_available = registry_.has("read_skill");
    sources.memory = retriever_.get();
    sources.skills = skills_.get();
    sources.activated = &activated_skills_;
    sources.brief = cfg_.session_brief ? &brief_store_ : nullptr;
    sources.prompts = prompt_registry_;
    return sources;
}

void Agent::ensure_system_prompt() {
    std::string system = render_system_prompt();
    if (context_.empty()) {
        push_system_prompt(std::move(system));
        return;
    }
    // The prompt's inputs can change mid-session: a plugin toggle changes which
    // tools exist, and the schema follows at once. The prose has to follow too,
    // or the model is told about a tool it cannot call. Rebuilding costs the KV
    // prefix only when the text actually changed.
    const auto& first = context_.get_all().front();
    if (first.role != "system" || first.content == system)
        return;
    const auto rest = context_.get_all();
    context_.clear();
    push_system_prompt(std::move(system));
    for (std::size_t i = 1; i < rest.size(); ++i)
        context_.push(rest[i]);
    emit_context_event(context_events_, context_);
}

void Agent::push_system_prompt(std::string system) {
    Message sys_msg;
    sys_msg.role = "system";
    sys_msg.content = std::move(system);
    context_.push(std::move(sys_msg));
    emit_context_event(context_events_, context_);
    publish_message_added(context_.get_all().back());
}

std::string Agent::learn_forget(const std::string& id) {
    if (!memory_store_ || experience_cfg_.store_path.empty())
        return "experience store disabled";
    if (!memory_store_->remove(id))
        return "no learned item with id '" + id + "'";
    memory_store_->save(experience_cfg_.store_path);
    return "";
}

std::string Agent::learn_pin(const std::string& id, bool pinned) {
    if (!memory_store_ || experience_cfg_.store_path.empty())
        return "experience store disabled";
    if (!memory_store_->set_promoted(id, pinned))
        return "no learned item with id '" + id + "'";
    memory_store_->save(experience_cfg_.store_path);
    return "";
}

void Agent::set_context(std::vector<Message> messages) {
    context_.clear();
    for (auto& m : messages)
        context_.push(std::move(m));
    emit_context_event(context_events_, context_);
}

void Agent::fork_from(const IAgent& src_port) {
    // The port is implemented by Agent in every host; a foreign implementation
    // has no conversation to copy and is ignored rather than guessed at.
    const auto* source = dynamic_cast<const Agent*>(&src_port);
    if (!source)
        return;
    const Agent& src = *source;
    // The message stack copies through the integrity-checked snapshot; the
    // seal/hash chain is rebuilt by set_context, so both legs keep a valid
    // chain and diverge independently afterward.
    const auto& all = src.context_.get_all();
    set_context(std::vector<Message>(all.begin(), all.end()));
    cfg_ = src.cfg_;
    // The fork is its own run: cancelling it must never touch the parent.
    cfg_.cancel_token = CancellationToken{};
    publish_config();
    meta_ = src.meta_;
    session_approved_ = src.session_approved_;
    model_windows_ = src.model_windows_;
    turn_counter_ = src.turn_counter_;
    user_turn_counter_ = src.user_turn_counter_;
    brief_store_ = src.brief_store_;
    activated_skills_.assign(src.activated_skills_.snapshot());
    compression_controller_->set_last(src.compression_controller_->last());
    experience_cfg_ = src.experience_cfg_;
    // A fork stays on the parent's project: adopt its shared catalog and
    // store so learned skills/memories stay consistent across both legs.
    if (src.skills_)
        skills_ = src.skills_;
    if (src.memory_store_) {
        memory_store_ = src.memory_store_;
        retriever_ = std::make_unique<MemoryRetriever>(*memory_store_, experience_cfg_);
    }
    // Rebuild the client from the copied config so the fork's first request
    // serializes the identical wire prefix (KV-cache reuse); hooks, log,
    // and event wiring stay the fork's own.
    client_ = make_client(cfg_, client_factory_);
}

// Compression gate: if it triggers, compress and persist, then rebuild the
// prompt from the new context for this call. Returns true when it rebuilt.
bool Agent::maybe_compress(std::vector<Message>& prompt_copy,
                           const std::vector<std::shared_ptr<Tool>>& tools) {
    if (!compression_controller_->auto_enabled())
        return false;
    resolve_window();
    if (!compression_controller_->should_compress(context_, cfg_))
        return false;

    const auto decision = compression_controller_->last_decision();
    if (hooks_.on_debug) {
        hooks_.on_debug("gate: tokens=" + std::to_string(static_cast<long>(decision.tokens)) +
                        " window=" + std::to_string(static_cast<long>(decision.budget)) +
                        " threshold=" + std::to_string(decision.threshold));
    }
    // Structured replacement for the on_status prose a UI had to parse:
    // subscribers get the numbers the gate actually decided on.
    CompressionEvent triggered;
    triggered.tokens = static_cast<long>(decision.tokens);
    triggered.budget = static_cast<long>(decision.budget);
    triggered.threshold = decision.threshold;
    publish_event(triggered);
    // The live request prefix is captured BEFORE the rebuild: the classify call
    // must replay exactly what the last turn sent (blocks + tool schemas), or the
    // server re-prefills the whole conversation instead of extending its cache.
    const CompressionRequestPrefix prefix = compression_prefix(tools);
    if (!run_compression(std::function<void()>(), nullptr, &prefix))
        return false;
    // Build prompt_copy from the new compressed context for the current LLM call.
    auto new_msgs = context_.get_all();
    prompt_copy.assign(new_msgs.begin(), new_msgs.end());
    return true;
}

namespace {
// The smallest saving worth the rebuild a changed prefix forces. Below this the
// model asked for a prefill and would get nothing back for it.
constexpr std::size_t kShapeMinFreedBytes = 2048;
} // namespace

bool Agent::apply_pending_shapes(std::vector<Message>& prompt_copy) {
    if (!shape_queue_ || shape_queue_->empty())
        return false;
    const std::vector<ShapePlan> plans = shape_queue_->drain();
    if (last_shape_turn_ == user_turn_counter_ && user_turn_counter_ != 0) {
        inject_harness_note("[harness] context shaping already ran this turn; "
                            "the next plan applies after the next user message.");
        return false;
    }
    const auto msgs = context_.get_all();
    std::vector<Message> next(msgs.begin(), msgs.end());
    ShapeOutcome total;
    for (const ShapePlan& plan : plans) {
        const ShapeOutcome one = apply_shape_plan(next, plan);
        total.applied += one.applied;
        total.freed_bytes += one.freed_bytes;
        total.rejected.insert(total.rejected.end(), one.rejected.begin(), one.rejected.end());
    }
    if (total.applied == 0 || total.freed_bytes < kShapeMinFreedBytes) {
        inject_harness_note("[harness] context shaping freed " + std::to_string(total.freed_bytes) +
                            " bytes from " + std::to_string(total.applied) +
                            " result(s); below the " + std::to_string(kShapeMinFreedBytes) +
                            "-byte floor, so nothing was removed.");
        return false;
    }
    last_shape_turn_ = user_turn_counter_;
    rebuild_context(next);
    prompt_copy.assign(next.begin(), next.end());
    std::string note = "[harness] context shaping replaced " + std::to_string(total.applied) +
                       " result(s) (" + std::to_string(total.freed_bytes) +
                       " bytes) with recallable digests; read_result restores any of them.";
    if (!total.rejected.empty())
        note += " Ignored " + std::to_string(total.rejected.size()) + " unknown id(s).";
    inject_harness_note(note);
    return true;
}

// Surface streamed reasoning and tokens to the host.
void Agent::on_stream_chunk(const AgentHooks& h, const StreamChunk& ch) const {
    if (ch.done)
        return;
    if (!ch.reasoning.empty()) {
        if (h.on_state)
            h.on_state(RunState::Thinking);
        if (h.on_reasoning)
            h.on_reasoning(ch.reasoning);
    }
    if (!ch.delta.empty()) {
        if (h.on_state)
            h.on_state(RunState::Streaming);
        if (h.on_token)
            h.on_token(ch.delta);
    }
}

// Report the exchange's token stats to the host, and keep the running prompt
// count for the next request.
void Agent::record_stats(const Stats& stats) {
    if (!stats.valid)
        return;
    if (hooks_.on_stats)
        hooks_.on_stats(stats);
    if (hooks_.on_usage)
        hooks_.on_usage({UsageSource::Main,
                         stats.prompt_tokens,
                         stats.completion_tokens,
                         stats.cache_read_tokens,
                         stats.cache_write_tokens,
                         {},
                         {},
                         stats.reasoning_tokens});
    if (!stats.cache_miss_reason.empty())
        log_.event("cache_miss", {{"reason", stats.cache_miss_reason}});
    if (stats.prompt_tokens > 0)
        cfg_.prompt_tokens_used = stats.prompt_tokens;
}

// Every completed exchange is visible to subscribers: the loop has no hidden
// round-trips any more, so nothing has to be filtered out of the event stream.
void Agent::publish_llm_response(const Stats& stats) {
    LlmResponseEvent response;
    response.status = 200;
    if (stats.valid) {
        response.prompt_tokens = stats.prompt_tokens;
        response.completion_tokens = stats.completion_tokens;
    }
    response.cache_read_tokens = stats.cache_read_tokens;
    response.cache_write_tokens = stats.cache_write_tokens;
    response.reasoning_tokens = stats.reasoning_tokens;
    response.cache_miss_reason = stats.cache_miss_reason;
    response.owner_id = owner_id_;
    response.model = cfg_.model;
    response.provider = cfg_.provider_name;
    publish_event(response);
}

Message Agent::chat_once(const std::vector<std::shared_ptr<Tool>>& tools) {
    Message reply;
    Stats stats;
    if (hooks_.on_debug)
        hooks_.on_debug("chat: request");
    if (hooks_.on_state)
        hooks_.on_state(RunState::Waiting);

    // Build prompt from the immutable context stack (copy for augmentation).
    auto prompt_msgs = context_.get_all();
    std::vector<Message> prompt_copy(prompt_msgs.begin(), prompt_msgs.end());

    // Sync the turn counters so the compression gate can check cooldown.
    cfg_.turn_counter = turn_counter_;
    cfg_.user_turn_counter = user_turn_counter_;

    // Check compression gate. If triggered, compress and persist.
    maybe_compress(prompt_copy, tools);

    // A plan the agent submitted between requests is applied here, at the
    // boundary, so the history and the request agree.
    apply_pending_shapes(prompt_copy);

    // Every injected block is assembled here, ONCE, and only after the gate.
    // Injection used to be spread across four sites above and below this point,
    // and the one above was silently discarded whenever the rebuild replaced
    // the prompt — which is precisely when a long session needs its retrieved
    // memories. There is now no "before the gate" to inject into by accident.
    PromptAssembler::inject_blocks(prompt_sources(), prompt_copy);

    if (cfg_.stream) {
        reply = client_->chat_stream(
            prompt_copy, tools, [this](const StreamChunk& ch) { on_stream_chunk(hooks_, ch); },
            &stats);
    } else {
        reply = client_->chat(prompt_copy, tools, &stats);
    }
    record_stats(stats);
    publish_llm_response(stats);

    // Whatever the client had to do to the request (a window the server
    // clamped, calls it had to drop) reaches the model before its next word.
    for (const auto& note : client_->take_notices())
        inject_harness_note("[harness] " + note);

    ++turn_counter_;
    return reply;
}

void Agent::push_reply(Message reply) {
    // The caller extracts text-embedded tool calls before the dispatch
    // snapshot; here the (already structured) reply is sealed.
    if (!reply.reasoning.empty())
        log_.event("reasoning", {{"content", reply.reasoning}});
    context_.push(std::move(reply));
    emit_context_event(context_events_, context_);
    publish_message_added(context_.get_all().back());
}

bool Agent::extract_embedded_tool_calls(Message& reply) const {
    if (!(reply.tool_calls.is_null() || reply.tool_calls.empty()) || reply.content.empty())
        return false;
    auto extracted = extract_tool_calls_from_text(reply.content);
    if (extracted.is_null() || extracted.empty())
        return false;
    reply.tool_calls = std::move(extracted);
    reply.reasoning.clear();
    reply.content.clear();
    // NOTE: no on_tool_call here — dispatch fires it once per executed call
    // (dispatch.cpp); firing it here too doubles the UI's "running" lines.
    return true;
}

CompressionResult Agent::compress_now(std::function<void()> progress_cb) {
    CompressionResult r;
    run_compression(std::move(progress_cb), &r);
    return r;
}

// Tell the host a compression finished. The failure path reports the
// pre-compression token count, the success path the post-compression one.
void Agent::publish_compression_done(bool success, long tokens_after) {
    CompressionCompletedEvent finished;
    finished.success = success;
    finished.tokens_after = tokens_after;
    publish_event(finished);
}

// Spec invariant 7: a failed compression leaves the context untouched. Report
// the real error — a failure here must not masquerade as "no compressor
// configured" (the TUI keys off messages_before == 0).
bool Agent::fail_compression_after(CompressionResult& r, CompressionReporter& reporter,
                                   CompressionResult* out) {
    reporter.on_error(r.error);
    // The history stands (spec invariant 7), but the model is told the attempt
    // happened: a compression that failed changes how much room is left, and
    // that is the model's business too.
    inject_harness_note("[harness] context compression failed (" + r.error +
                        "); the conversation is unchanged and the window is unchanged.");
    if (hooks_.on_compression)
        hooks_.on_compression(r);
    publish_compression_done(false, static_cast<long>(r.tokens_before));
    publish_error("compression", r.error);
    if (out)
        *out = std::move(r);
    return false;
}

// Rebuild the live context from the compressed list, using only the stack
// primitives (clear + push). The pipeline itself never touched it.
void Agent::rebuild_context(std::vector<Message>& compressed) {
    context_.clear();
    for (auto& m : compressed)
        context_.push(std::move(m));
    emit_context_event(context_events_, context_);
}

RunScope Agent::make_run_scope() {
    RunScope scope;
    scope.cancel_token = &cfg_.cancel_token;
    scope.skills = skills_.get();
    scope.activated = &activated_skills_;
    scope.project_root = project_root_.empty() ? nullptr : &project_root_;
    scope.workspace_root = workspace_root_.empty() ? nullptr : &workspace_root_;
    scope.config = &cfg_;
    scope.owner_id = owner_id_.empty() ? nullptr : &owner_id_;
    scope.session_id = cfg_.session_id.empty() ? nullptr : &cfg_.session_id;
    return scope;
}

bool Agent::run_compression(std::function<void()> progress_cb, CompressionResult* out,
                            const CompressionRequestPrefix* prefix) {
    // Same scope as a turn: compression may run on its own worker thread,
    // where it still needs this agent's cancel token and catalog.
    // A worker's token belongs to the worker run: if it was already requested,
    // clearing it here would erase the cancel the executor is waiting on. A
    // top-level agent clears its own between turns.
    if (!in_subagent())
        cfg_.cancel_token.clear();
    RunScope scope = make_run_scope();
    ScopedRunScope scope_guard(scope);

    // A host that did not hand us the live prefix (manual /compress) can still
    // reproduce it: the injected blocks are derived from this context and the
    // tool set from the registry's snapshot.
    CompressionRequestPrefix live_prefix;
    if (!prefix) {
        live_prefix = compression_prefix(resolve_tools());
        prefix = &live_prefix;
    }

    // The reporter carries the progress stream; the controller computes the
    // authoritative result from the snapshot and the replacement list.
    CompressionReporter reporter(hooks_, std::move(progress_cb));

    const CompressionController::Request request{context_, *client_,  cfg_,
                                                 *prefix,  &reporter, user_turn_counter_};
    auto outcome = compression_controller_->run(request);
    // A refusal before the pipeline (no strategy, too short) is not an attempt:
    // the window did not move, so nothing is announced.
    if (!outcome.attempted) {
        if (out)
            *out = std::move(outcome.result);
        return false;
    }
    if (!outcome.ok)
        return fail_compression_after(outcome.result, reporter, out);

    complete_compression(outcome, reporter, out);
    return true;
}

// A pass that ran: rebuild the stack, apply what it learned, then tell the
// host. The controller already recorded the result.
void Agent::complete_compression(CompressionController::Outcome& outcome,
                                 CompressionReporter& reporter, CompressionResult* out) {
    rebuild_context(outcome.messages);
    // The cached server prompt count describes the PRE-compression context;
    // reset it so the gate evaluates the new context honestly (falls back to the
    // estimate until the next chat refreshes it).
    cfg_.prompt_tokens_used = -1;
    // Apply memory/skill ops from the LLM classification response, then the
    // session brief the extract step produced (non-fatal).
    apply_compression_result(outcome.response);
    apply_brief(outcome.response);

    reporter.on_compress_done(outcome.result);
    if (hooks_.on_compression)
        hooks_.on_compression(outcome.result);
    publish_compression_done(true, static_cast<long>(outcome.result.tokens_after));
    if (out)
        *out = std::move(outcome.result);
}

void Agent::log_and_push_user_prompt(const std::string& prompt) {
    // Diagnostics are opt-in and the sink is lazy: every prompt re-asserts it,
    // so a resumed session (transcript already open) still gets its journal.
    if (cfg_.debug)
        log_.open_errors(error_log_path());
    if (!log_.enabled()) {
        // Default-on transcript: nothing the agent saw is ever lost, even
        // when the user never configured log_path. Per-session file under the
        // project's state dir; {ts} expands to the session id.
        std::string path = cfg_.log_path;
        if (path.empty())
            path = Workspace::state_dir() + "/logs/{ts}.jsonl";
        log_.open(path);
    }
    log_.event("user", {{"content", prompt}, {"model", cfg_.model}});
    Message msg;
    msg.role = "user";
    msg.content = prompt;
    context_.push(std::move(msg));
    ++user_turn_counter_;
    emit_context_event(context_events_, context_);
    publish_message_added(context_.get_all().back());
}

std::vector<std::shared_ptr<Tool>> Agent::resolve_tools() {
    // Keep the snapshot's ownership leases: raw pointers into a temporary
    // snapshot would dangle the moment the registry mutates.
    return registry_.snapshot_tools();
}

// One LLM round-trip with graceful recovery for request-side 4xx failures:
// the retry loop can REPAIR the request once and retry. The ladder that decides
// how lives in repair.cpp; this file only bridges it to the agent's config,
// hooks and log, and maps the answer onto a retry.
class Agent::RepairBridge final : public RepairHost {
public:
    explicit RepairBridge(Agent& agent) : agent_(agent) {}

    std::string model() const override { return agent_.cfg_.model; }

    void report_error(const std::string& kind, const std::string& message) override {
        agent_.publish_error(kind, message);
    }

    void status(const std::string& line) override {
        if (agent_.hooks_.on_status)
            agent_.hooks_.on_status(line);
    }

    void announce(const std::string& note) override { agent_.inject_harness_note(note); }

    std::vector<ModelInfo> server_models() override { return list_model_info(agent_.cfg_); }

    void adopt_model(const std::string& id, const ModelCapabilities& caps) override {
        agent_.set_model(id, caps);
    }

    std::string request_api_key(const char* stage) override {
        if (!agent_.hooks_.on_api_key)
            return "";
        const std::string reason =
            "API key for provider '" + agent_.cfg_.provider_name + "' was rejected (HTTP 401/403)";
        if (agent_.hooks_.on_status)
            agent_.hooks_.on_status(reason + " - requesting a new key");
        const std::string key = agent_.hooks_.on_api_key(reason);
        if (!key.empty() && agent_.hooks_.on_debug)
            agent_.hooks_.on_debug("auth: key updated, retrying " + std::string(stage));
        return key;
    }

    void adopt_api_key(const std::string& key) override {
        agent_.set_connection(agent_.cfg_.api_base, key, agent_.cfg_.model);
    }

private:
    Agent& agent_;
};

ChatAdapter Agent::build_chat_adapter(const char* stage,
                                      const std::vector<std::shared_ptr<Tool>>& tools) {
    return [this, stage, &tools](const std::string& err) -> std::function<Message()> {
        RepairBridge host(*this);
        switch (repairs_.repair(err, stage, !tools.empty(), host)) {
        case RetryRequest::WithoutTools:
            return [this]() { return chat_once({}); };
        case RetryRequest::WithTools:
            return [this, &tools]() { return chat_once(tools); };
        case RetryRequest::None:
            break;
        }
        return {};
    };
}

Message Agent::chat_with_recovery(const std::vector<std::shared_ptr<Tool>>& tools,
                                  const char* stage) {
    const ChatAdapter adapt = build_chat_adapter(stage, tools);
    auto chat = [this, &tools]() { return chat_once(tools); };
    try {
        Message reply = chat_with_retry(hooks_, log_, chat, stage, cfg_.cancel_token, 3, adapt);
        // Retry exhaustion is returned as a harness notice, not thrown: publish
        // it once here, so the failure reaches the host and the plugins before
        // the model is asked to decide the next move.
        if (!reply.failure_detail.empty())
            publish_error("transport", reply.failure_detail);
        return reply;
    } catch (const std::exception& e) {
        const auto* api = dynamic_cast<const ApiError*>(&e);
        publish_error("transport", e.what(), api ? api->retryable : false);
        throw;
    }
}

// Cancellation path: returns a true empty reply (no empty-turn
// substitution — the stop was deliberate) and skips any further work.
std::string Agent::finish_turn_cancelled() {
    log_.event("turn_end", {{"reason", "cancelled"}});
    if (hooks_.on_state)
        hooks_.on_state(RunState::Idle);
    TurnEndedEvent ended;
    ended.cancelled = true;
    publish_event(ended);
    return "";
}

std::string Agent::finish_turn(Turn& turn) {
    std::string final_reply = turn.take_reply();
    if (final_reply.empty()) {
        const char* reason = "empty_reply";
        if (turn.ran_tools())
            reason = "empty_after_tools";
        else if (!turn.last_failure().empty())
            reason = "empty_after_failure";
        final_reply = empty_turn_reply(turn.ran_tools(), turn.last_failure());
        log_.event("error", {{"reason", reason}});
    }
    if (!turn.closed_by_model()) {
        // A bound ended this turn, not the model. The reason goes into the
        // conversation so the next turn opens knowing why it stopped, and into
        // the host's hands so the stop is never silent on screen.
        inject_harness_note("[harness] the turn ended without a closing reply: " + final_reply);
    }
    // The closing text is announced and recorded here — the single exit of every
    // non-cancelled turn. A streamed reply is already on screen (the host skips
    // a duplicate), a stop reason is not.
    if (hooks_.on_assistant)
        hooks_.on_assistant(final_reply);
    log_.event("assistant", {{"content", final_reply}});
    log_.event("turn_end", {{"content", final_reply}});
    if (hooks_.on_state)
        hooks_.on_state(RunState::Idle);
    TurnEndedEvent ended;
    publish_event(ended);
    return final_reply;
}

namespace {

// How a prompt block is carried on the wire. Head blocks are system messages
// beside the prompt; tail blocks ride the user channel with the harness flag,
// after the conversation, so their churn cannot rewrite the cached prefix. The
// compression replay uses the same mapping, or its request would not share the
// live prefix it exists to reuse. Order is the caller's (sorted before
// conversion).
std::vector<Message> block_messages(const std::vector<PromptBlock>& blocks, bool tail) {
    std::vector<Message> out;
    out.reserve(blocks.size());
    for (const auto& block : blocks) {
        Message m;
        m.role = tail ? "user" : "system";
        m.harness = tail;
        m.content = block.text;
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace

CompressionRequestPrefix
Agent::compression_prefix(const std::vector<std::shared_ptr<Tool>>& tools) const {
    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(prompt_sources(), head, tail);
    PromptAssembler::order_blocks(head);
    PromptAssembler::order_blocks(tail);
    CompressionRequestPrefix prefix;
    prefix.head_blocks = block_messages(head, /*tail=*/false);
    prefix.tail_blocks = block_messages(tail, /*tail=*/true);
    prefix.tools = tools;
    return prefix;
}

// Wall-clock budget: the engine enforces max_wall_ms, not just the post-hoc
// scorer — a runaway must stop at the deadline. Returns true when the turn ends.
bool Agent::turn_budget_exceeded(Turn& turn) {
    if (!turn.wall_clock_exceeded(std::chrono::steady_clock::now()))
        return false;
    if (hooks_.on_status)
        hooks_.on_status("wall-clock budget exceeded, stopping");
    log_.event("error", {{"reason", "wall_clock_exceeded"}});
    turn.stop_with("[stopped: wall-clock budget exceeded; simplify the task or retry]");
    return true;
}

// One reply that carries tool calls: report repeated narration (opt-in guard),
// track the canonical call for the repeat note, dispatch. Returns true when the
// loop must stop.
bool Agent::fold_tool_calls(Turn& turn, const json& tool_calls, const std::string& content) {
    turn.record_tool_reply();
    if (loop_guard_->repeated_narration(content, turn))
        return true;
    // The repeat counter is the reporting one (canonical arguments, timing
    // stripped); it never intervenes, it just feeds the loop-state note.
    turn.observe_repeated_call(canonical_call_key(tool_calls));
    loop_guard_->announce_dispatch(tool_calls, content);
    const bool ok =
        dispatch_tool_calls(DispatchRequest{tool_calls, cfg_, registry_, hooks_, log_,
                                            session_approved_, &policy_, event_bus_, context_});
    if (!loop_guard_->stop_dispatch_loop(tool_calls, turn))
        loop_guard_->fold_fail_streak(tool_calls, ok, turn);
    return turn.has_reply();
}

// A report from the environment is not the model going quiet: the model has the
// next word. Three in a row mean the endpoint is down.
bool Agent::fold_harness_notice(Turn& turn) {
    return turn.record_notice();
}

// A silent empty reply. The model may have lost the thread, so it gets one more
// round with a note; after that the turn stops.
bool Agent::fold_silent_reply(Turn& turn) {
    if (turn.record_silent_reply())
        return true;
    inject_harness_note("[harness] the previous reply carried no tool call and no content. If work "
                        "remains, continue with a tool or state what blocks it; otherwise close "
                        "with the result.");
    return false;
}

// Fold one iteration's reply into the turn. Returns true when the loop should
// stop; the reply is pushed either way.
//
// The turn contract: the MODEL decides when the turn ends. Tool calls mean
// "mid-process, keep going"; a reply with no tool calls is the answer, and the
// loop stops there. The harness never asks for another round and never invents
// a turn of its own; its own reports ([harness] notices) hand the model the
// next word instead of closing the turn.
bool Agent::advance_turn(Turn& turn, const std::vector<std::shared_ptr<Tool>>& tools,
                         Message& reply) {
    (void)tools;
    // Extract tool_calls and content before push_reply (which moves reply).
    // Template-style tool calls arrive embedded in the content (e.g. ornith's
    // <tool_call><function=...><parameter=...>) — extract them BEFORE the
    // dispatch snapshot so they actually execute.
    extract_embedded_tool_calls(reply);
    const json tc = reply.tool_calls;
    const std::string content = reply.content;
    const bool notice = reply.harness;
    const std::string failure = reply.failure_detail;
    push_reply(std::move(reply));

    if (!tc.is_null() && !tc.empty())
        return fold_tool_calls(turn, tc, content);
    if (notice) {
        if (!failure.empty())
            turn.record_failure(failure);
        // A rate-limit notice is the harness speaking, exactly like a retry
        // report: the model gets the next word, not a dead turn. The governor
        // has already waited; if it is still limited, wait/throttle are the
        // model's tools, not a reason to drop the session.
        return fold_harness_notice(turn);
    }
    if (content.empty())
        return fold_silent_reply(turn);

    turn.close_with(content); // the model's own reply is the turn's close
    return true;
}

bool Agent::fold_reply(Turn& turn, const std::vector<std::shared_ptr<Tool>>& tools,
                       Message& reply) {
    try {
        return advance_turn(turn, tools, reply);
    } catch (const CancelledError&) {
        throw; // a deliberate stop, not a failure to recover from
    } catch (const std::exception& e) {
        return recover_internal_failure(turn, e.what());
    }
}

// Push a harness notice into the conversation. It rides the user channel with
// the harness flag set: the model sees it in place, the loop never mistakes it
// for an answer, and a UI can render it as the environment speaking.
void Agent::inject_harness_note(const std::string& text) {
    Message note;
    note.role = "user";
    note.harness = true;
    note.content = text;
    context_.push(std::move(note));
    emit_context_event(context_events_, context_);
    publish_message_added(context_.get_all().back());
}

void Agent::prepare_iteration(Turn& turn, int iter) {
    loop_guard_->note_state(turn, iter);
    // Safe boundary: everything the environment finished since the last request
    // enters the conversation once, before the next request.
    drain_inbox();
}

void Agent::drain_inbox() {
    if (!inbox_)
        return;
    auto completions = inbox_->take(owner_id_);
    if (completions.empty())
        return;
    std::string text = format_task_digest(completions);
    if (text.empty())
        return;
    Message msg;
    msg.role = "user";
    msg.harness = true;
    msg.content = std::move(text);
    context_.push(std::move(msg));
    emit_context_event(context_events_, context_);
    publish_message_added(context_.get_all().back());
    std::vector<std::string> ids;
    ids.reserve(completions.size());
    for (const auto& completion : completions)
        ids.push_back(completion.task_id);
    inbox_->ack(owner_id_, ids);
}

bool Agent::recover_internal_failure(Turn& turn, const std::string& what) {
    log_.event("error", {{"reason", "internal_failure"}, {"error", what}});
    if (hooks_.on_status)
        hooks_.on_status("internal error (recovered): " + what);
    inject_harness_note(internal_failure_reply(what));
    answer_orphan_tool_calls(what);
    if (!turn.record_internal_failure())
        return false;
    turn.stop_with("[stopped: repeated internal errors — " + what + "]");
    return true;
}

void Agent::answer_orphan_tool_calls(const std::string& what) {
    std::set<std::string> answered;
    const json* calls = nullptr;
    for (const auto& m : context_.get_all()) {
        if (m.role == "tool" && !m.tool_call_id.empty())
            answered.insert(m.tool_call_id);
        if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty())
            calls = &m.tool_calls;
    }
    if (calls == nullptr)
        return;
    std::vector<std::string> missing;
    for (const auto& call : *calls) {
        const std::string id = read_string(call, "id", "");
        if (!id.empty() && answered.count(id) == 0)
            missing.push_back(id);
    }
    for (const auto& id : missing) {
        Message msg;
        msg.role = "tool";
        msg.tool_call_id = id;
        msg.content = "[tool call failed internally: " + what + "]";
        context_.push(std::move(msg));
        emit_context_event(context_events_, context_);
        publish_message_added(context_.get_all().back());
    }
}

// Resolve the model if the host started without one, then fire the turn's
// opening event — before the prompt is sealed into the context, so an
// interceptor can still rewrite what the model will see.
void Agent::open_turn(const std::string& user_prompt) {
    // The host may have started without a resolved model: interactive
    // startup reads the model catalog from cache only and revalidates it in
    // the background, so a cold first launch can reach here with no model.
    // This is the agent worker — a bounded cache-through fetch is legal here,
    // and a failure leaves the model empty for the normal error path.
    if (cfg_.model.empty() && apply_server_autodetect(cfg_).ok) {
        client_ = make_client(cfg_, client_factory_);
        publish_config();
    }
    ensure_system_prompt();
    TurnStartedEvent turn_start;
    turn_start.prompt = user_prompt;
    publish_event(turn_start);
    log_and_push_user_prompt(turn_start.prompt);
}

std::string Agent::run(const std::string& user_prompt) {
    // Fresh turn: a cancel requested before this run must not abort it. The
    // exception is a worker, whose token may already be requested by the task
    // inventory before the child starts.
    if (!in_subagent())
        cfg_.cancel_token.clear();
    // Install this agent's run scope so shared tools resolve OUR cancel
    // token, skill catalog, and activation sink — not whichever objects
    // were bound at registration.
    RunScope scope = make_run_scope();
    ScopedRunScope scope_guard(scope);
    open_turn(user_prompt);

    const auto tools = resolve_tools();
    Turn turn(std::chrono::steady_clock::now(), cfg_.max_wall_ms);

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        if (turn_budget_exceeded(turn))
            break;
        // Cancellation ends the turn cleanly: no fabricated error message,
        // no probe round-trip, nothing pushed into the context.
        if (run_cancelled(cfg_.cancel_token)) {
            if (hooks_.on_status)
                hooks_.on_status("cancelled by user");
            return finish_turn_cancelled();
        }
        if (hooks_.on_debug)
            hooks_.on_debug("iteration " + std::to_string(iter + 1) + "/" +
                            std::to_string(cfg_.max_tool_iterations));
        prepare_iteration(turn, iter);

        Message reply;
        try {
            reply = chat_with_recovery(tools, "generation");
        } catch (const CancelledError&) {
            if (hooks_.on_status)
                hooks_.on_status("cancelled by user");
            return finish_turn_cancelled();
        }
        if (fold_reply(turn, tools, reply))
            break;
    }
    return finish_turn(turn);
}

// Upserts vs deprecations in one op list.
struct OpCounts {
    size_t upserts = 0;
    size_t deprecations = 0;
};

OpCounts count_ops(const std::vector<KnowledgeOp>& ops) {
    OpCounts c;
    for (const auto& op : ops)
        (op.action == "deprecate" ? c.deprecations : c.upserts)++;
    return c;
}

// "N memories upserted, M deprecated, ..." — empty when nothing was applied.
std::string ops_summary(const OpCounts& mem, const OpCounts& skill) {
    std::string summary;
    const auto add = [&summary](size_t n, const char* what) {
        if (n)
            summary += (summary.empty() ? "" : ", ") + std::to_string(n) + " " + what;
    };
    add(mem.upserts, "memories upserted");
    add(mem.deprecations, "deprecated");
    add(skill.upserts, "skills upserted");
    add(skill.deprecations, "deprecated");
    return summary;
}

// Decay the store, persist it, and report what was evicted.
void Agent::decay_and_persist() {
    const size_t before_decay = memory_store_->store_size();
    memory_store_->decay_all();
    const size_t after_decay = memory_store_->store_size();
    if (!experience_cfg_.store_path.empty())
        memory_store_->save(experience_cfg_.store_path);
    if (!hooks_.on_status)
        return;
    const size_t pruned = (before_decay > after_decay) ? before_decay - after_decay : 0;
    if (pruned > 0)
        hooks_.on_status("decay: " + std::to_string(pruned) + " items evicted (" +
                         std::to_string(before_decay) + " → " + std::to_string(after_decay) +
                         " total)");
}

void Agent::apply_compression_result(const CompressionResponse& cr) {
    if (!memory_store_ || experience_cfg_.store_path.empty())
        return;
    // A cycle with no ops is still a cycle: decay runs unconditionally, so
    // knowledge that was not reconfirmed loses weight. An empty store has
    // nothing to decay, so it is the one case that writes nothing.
    const bool has_ops = !cr.memory_ops.empty() || !cr.skill_ops.empty();
    if (!has_ops && memory_store_->store_size() == 0)
        return;

    memory_store_->set_current_turn(turn_counter_);

    const OpCounts mem = count_ops(cr.memory_ops);
    std::vector<ExtractionItem> items;
    if (!cr.memory_ops.empty())
        apply_memory_ops(*memory_store_, cr.memory_ops, experience_cfg_.store_path, &items);

    const OpCounts skill = count_ops(cr.skill_ops);
    if (!cr.skill_ops.empty())
        apply_skill_ops(*memory_store_, cr.skill_ops, experience_cfg_.store_path, &items);

    decay_and_persist();

    // A skill learned by this cycle is usable now, not after a restart. The
    // catalog is shared per project, so every window on it sees the new entry.
    if (skills_ && memory_store_)
        skills_->discover(memory_store_->top_skills(experience_cfg_.max_skills, ""));

    if (!hooks_.on_status)
        return;
    const std::string summary = ops_summary(mem, skill);
    if (!summary.empty())
        hooks_.on_status("extraction: " + summary);
    hooks_.on_status("memory store: " + std::to_string(memory_store_->store_size()) +
                     " total (memories + skills)");
}

void Agent::apply_brief(const CompressionResponse& cr) {
    if (!cr.brief)
        return;
    brief_store_.merge(*cr.brief);
    if (hooks_.on_status && !brief_store_.empty())
        hooks_.on_status("session brief: updated (intent/direction/done/next/avoid)");
}

} // namespace agent
