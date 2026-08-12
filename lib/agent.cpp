
#include "agent/agent.h"
#include "agent/environment.h"
#include "agent/plugin.h"
#include "agent/prompt.h"
#include "agent/tool_call_parser.h"
#include "agent/agent_helpers.h"
#include "agent/tool_recovery.h"
#include "agent/dispatch.h"
#include "agent/tools.h"
#include "agent/model_probe.h"
#include "agent/data_path.h"
#include "agent/workspace.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace agent {

namespace {

// Locate an optional prompt file: next to the binary first (works when the
// process CWD differs, e.g. the benchmark runner's workspace), then
// CWD-relative (dev runs from the repo root).
std::string optional_prompt(const char* name) {
    const std::string dir = exe_dir();
    if (!dir.empty()) {
        const std::string p = dir + "/" + name;
        if (file_exists(p)) return p;
    }
    return name;
}

// Helper: publish context-change event to all subscribers.
void emit_context_event(ContextEventSource& src, const Context& ctx) {
    src.publish(ctx.token_count(), ctx.size());
}

// Build an LLM client for `cfg` through the injected factory, falling back
// to the real HttpLLMClient when no factory was provided.
std::unique_ptr<LLMClient> make_client(const Config& cfg,
                                       const LLMClientFactory& factory) {
    return factory ? factory(cfg) : std::make_unique<HttpLLMClient>(cfg);
}

} // namespace

Agent::Agent(Config cfg, ToolRegistry& registry, AgentHooks hooks,
             std::unique_ptr<CompressionStrategy> compressor,
             std::unique_ptr<CompressionGate> gate,
             std::unique_ptr<MemoryStore> memory_store,
             std::unique_ptr<MemoryRetriever> retriever,
             std::unique_ptr<LLMClient> client,
             LLMClientFactory client_factory,
             bool register_skills)
    : cfg_(std::move(cfg)), registry_(registry),
      client_factory_(std::move(client_factory)),
      client_(nullptr),
      hooks_(std::move(hooks))
    , compression_(std::move(compressor))
    , gate_(std::move(gate))
    , memory_store_(std::move(memory_store))
    , retriever_(std::move(retriever)) {
    if (client)
        client_ = std::move(client);
    else
        client_ = make_client(cfg_, client_factory_);
    experience_cfg_ = load_experience_config(cfg_);
    skills_ = std::make_unique<SkillCatalog>(cfg_);
    std::vector<Skill> learned;
    if (memory_store_) {
        auto top = memory_store_->top_skills(experience_cfg_.max_skills, "");
        learned.assign(top.begin(), top.end());
    }
    skills_->discover(learned);
    if (register_skills) register_skill_tools(registry_, *skills_);
}

void Agent::set_model(const std::string& model, int window) {
    cfg_.model = model;
    cfg_.model_explicit = true;
    if (window > 0) model_windows_[model] = window;
    client_ = make_client(cfg_, client_factory_);
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
        if (learned > 0 &&
            (cfg_.context_size == 0 || learned < cfg_.context_size))
            cfg_.context_size = learned;
    }
}

void Agent::set_reasoning_effort(const std::string& effort) {
    cfg_.reasoning_effort = effort;
    client_ = make_client(cfg_, client_factory_);
}

void Agent::set_connection(const std::string& api_base,
                           const std::string& api_key,
                           const std::string& model) {
    cfg_.api_base = api_base;
    if (!api_key.empty()) cfg_.api_key = api_key;
    if (!model.empty()) {
        cfg_.model = model;
        cfg_.model_explicit = true;
    }
    client_ = make_client(cfg_, client_factory_);
}

void Agent::ensure_system_prompt() {
    if (!context_.empty()) return;

    std::string system = load_prompt(cfg_.system_prompt_path);
    if (system.empty())
        throw std::runtime_error("system prompt file not found or empty: " +
                                 cfg_.system_prompt_path);
    // Environment card: OS, user, resources, available tools — collected
    // once at session start, so the agent can act in its environment without
    // probing. Session-fixed, so the KV prefix stays stable.
    std::string env_card = render_environment_card(probe_environment());
    if (!env_card.empty()) system += "\n\n" + env_card;
    if (!cfg_.tools_prompt_path.empty()) {
        std::string tools = load_prompt(cfg_.tools_prompt_path);
        if (tools.empty())
            throw std::runtime_error("tools prompt file not found or empty: " +
                                     cfg_.tools_prompt_path);
        system += "\n\n" + tools;
    } else if (!registry_.empty()) {
        system += "\n\n" + render_tools_markdown(registry_);
    }
    // Plugins: enabled plugin tools (registered as plugin_<id>_<name>) get
    // their own reference section so the agent knows they exist and how to
    // use them without touching the static tools.md.
    std::string plugins = plugin_tools_advertisement(registry_);
    if (!plugins.empty()) system += "\n\n" + plugins;

    switch (cfg_.mode) {
    case agent::AgentMode::Read:
        system += "\n\nYou are in READ mode. You can only use read-only tools "
                  "(search, grep, read). Writing files or running shell commands "
                  "is disallowed. Answer questions about the codebase but do not "
                  "make changes.";
        break;
    case agent::AgentMode::Write:
        system += "\n\nYou are in WRITE mode. All tools run without interactive "
                  "approval. You can read, edit, create files, and run commands. "
                  "You are trusted to modify the project.";
        break;
    case agent::AgentMode::Yolo:
        system += "\n\nYou are in YOLO mode. All tools run without approval and "
                  "execute immediately with full system access. The user trusts "
                  "you completely.";
        break;
    }

    // Optional git workflow prompt
    std::string git_path = cfg_.git_prompt_path.empty()
        ? "prompts/git.md" : cfg_.git_prompt_path;
    std::string git = load_prompt(git_path);
    if (!git.empty())
        system += "\n\n" + git;

    // Optional skills prompt (discovery block, authoring rule, trust boundary).
    // Resolved via the binary dir so the sections load regardless of CWD
    // (the benchmark runner runs with the workspace as CWD).
    std::string skills = load_prompt(optional_prompt("prompts/skills.md"));
    if (!skills.empty())
        system += "\n\n" + skills;

    // Optional MCP prompt (untrusted-server posture, user-only prompts).
    std::string mcp = load_prompt(optional_prompt("prompts/mcp.md"));
    if (!mcp.empty())
        system += "\n\n" + mcp;

    // Optional planning-tool prompt — only when the todowrite tool is enabled.
    if (cfg_.plan_tool) {
        std::string plan =
            load_prompt(optional_prompt("prompts/tools_planning.md"));
        if (!plan.empty()) system += "\n\n" + plan;
    }

    Message sys_msg;
    sys_msg.role = "system";
    sys_msg.content = system;
    context_.push(std::move(sys_msg));
    emit_context_event(context_events_, context_);
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

Message Agent::chat_once(const std::vector<std::shared_ptr<Tool>>& tools, bool display) {
    Message reply;
    Stats stats;
    if (hooks_.on_debug) hooks_.on_debug("chat: request");
    if (hooks_.on_state) hooks_.on_state(RunState::Waiting);

    // Build prompt from the immutable context stack (copy for augmentation).
    auto prompt_msgs = context_.get_all();
    std::vector<Message> prompt_copy(prompt_msgs.begin(), prompt_msgs.end());

    // Sync the turn counter so the compression gate can check cooldown.
    cfg_.turn_counter = turn_counter_;

    // Inject memories as a SEPARATE message (never modify system prompt).
    // This keeps the KV prefix stable for llama.cpp cache-prompt reuse.
    if (retriever_) {
        std::string user_msg;
        for (const auto& m : prompt_copy)
            if (m.role == "user") { user_msg = m.content; break; }
        auto suffix = retriever_->build_system_prompt_suffix(user_msg, 500);
        if (!suffix.empty()) {
            for (size_t i = 0; i < prompt_copy.size(); ++i) {
                if (prompt_copy[i].role == "system") {
                    Message knowledge;
                    knowledge.role = "system";
                    knowledge.content = suffix;
                    prompt_copy.insert(prompt_copy.begin() + static_cast<ptrdiff_t>(i + 1),
                                       std::move(knowledge));
                    break;
                }
            }
        }
    }

    // Check compression gate. If triggered, compress and persist.
    if (gate_ && compression_) {
        resolve_window();
        if (gate_->should_compress(context_, cfg_)) {
            if (hooks_.on_debug) {
                double tokens = 0, budget = 0, threshold = 0;
                gate_->last_decision(tokens, budget, threshold);
                hooks_.on_debug("gate: tokens=" +
                                std::to_string(static_cast<long>(tokens)) +
                                " window=" +
                                std::to_string(static_cast<long>(budget)) +
                                " threshold=" + std::to_string(threshold));
            }
            if (run_compression(std::function<void()>(), nullptr)) {
                // Build prompt_copy from the new compressed context for
                // the current LLM call.
                auto new_msgs = context_.get_all();
                prompt_copy.assign(new_msgs.begin(), new_msgs.end());
            }
        }
    }

    // Inject the skill discovery block as its own system slot on the prompt
    // copy, directly after the learned-knowledge slot. Keeps the stable prefix
    // system -> knowledge -> discovery; bodies load only on read_skill.
    if (skills_) {
        auto block = skills_->discovery_block();
        if (!block.empty()) {
            std::string disc = "Available skills (activate with read_skill):\n";
            for (const auto& line : block) disc += line + "\n";
            size_t pos = 0;
            for (size_t i = 0; i < prompt_copy.size(); ++i) {
                if (prompt_copy[i].role == "system") { pos = i + 1; break; }
            }
            if (pos < prompt_copy.size() && prompt_copy[pos].role == "system")
                ++pos;
            if (pos <= prompt_copy.size()) {
                Message disc_msg;
                disc_msg.role = "system";
                disc_msg.content = disc;
                prompt_copy.insert(
                    prompt_copy.begin() + static_cast<ptrdiff_t>(pos),
                    std::move(disc_msg));
            }
        }
        // Append session-activated skill bodies at the tail of the prompt
        // copy. Activation is rare and explicit, so the prefix is extended.
        for (const auto& act : skills_->activated_skills()) {
            Message body_msg;
            body_msg.role = "system";
            body_msg.content =
                "[activated skill: " + act.name + "]\n" + act.body;
            prompt_copy.push_back(std::move(body_msg));
        }
    }

    const AgentHooks& h = display ? hooks_ : silent_hooks();
    if (cfg_.stream) {
        reply = client_->chat_stream(prompt_copy, tools,
            [&h](const StreamChunk& ch) {
                if (ch.done) return;
                if (!ch.reasoning.empty()) {
                    if (h.on_state) h.on_state(RunState::Thinking);
                    if (h.on_reasoning) h.on_reasoning(ch.reasoning);
                }
                if (!ch.delta.empty()) {
                    if (h.on_state) h.on_state(RunState::Streaming);
                    if (h.on_token) h.on_token(ch.delta);
                }
            }, &stats);
    } else {
        reply = client_->chat(prompt_copy, tools, &stats);
    }
    if (stats.valid) {
        if (hooks_.on_stats) hooks_.on_stats(stats);
        if (stats.prompt_tokens > 0)
            cfg_.prompt_tokens_used = stats.prompt_tokens;
    }

    ++turn_counter_;
    return reply;
}

const AgentHooks& Agent::silent_hooks() const {
    static const AgentHooks silent = [] {
        AgentHooks h;
        h.on_state = [](RunState) {};
        return h;
    }();
    return silent;
}

void Agent::push_reply(Message reply) {
    // The caller extracts text-embedded tool calls before the dispatch
    // snapshot; here the (already structured) reply is sealed.
    if (!reply.reasoning.empty())
        log_.event("reasoning", {{"content", reply.reasoning}});
    context_.push(std::move(reply));
    emit_context_event(context_events_, context_);
}

bool Agent::extract_embedded_tool_calls(Message& reply) const {
    if (!(reply.tool_calls.is_null() || reply.tool_calls.empty()) ||
        reply.content.empty())
        return false;
    auto extracted = extract_tool_calls_from_text(reply.content);
    if (extracted.is_null() || extracted.empty()) return false;
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
    last_compression_ = r;
    return r;
}

bool Agent::run_compression(std::function<void()> progress_cb,
                            CompressionResult* out) {
    // Snapshot BEFORE compression — immutable, never mutate live stack.
    if (!compression_ || context_.size() < 2) return false;
    auto before = context_.get_all();
    size_t msgs_before = before.size();
    size_t tokens_before = context_.token_count();

    CompressionResult r;
    CompressionReporter reporter(hooks_, r, std::move(progress_cb));
    reporter.set_before(msgs_before, tokens_before);

    // Call pipeline on the LIVE context so both LLM calls extend the KV
    // cache from the same prefix (no full prefill between them). Pipeline
    // touches context_ only via push/pop (classify/extract requests); the
    // actual compressed result is applied below via pop-all + push.
    auto cc = load_compression_config(cfg_);
    CompressionResponse cr;
    auto compressed = compression_->compress(context_, cc, *client_,
                                              &reporter, &cr);

    // Cooldown applies to the attempt, not just the success: a failing
    // classifier must not re-fire the pipeline on every following turn and
    // burn two LLM calls each time — the gate stays silent for the cooldown
    // window and retries later.
    gate_->set_last_compress_turn(turn_counter_);

    // Spec invariant 7: a failed compression leaves the context untouched.
    if (!cr.error.empty()) {
        reporter.on_error(cr.error);
        return false;
    }

    // Rebuild context from compressed result using stack primitives.
    context_.clear();
    for (auto& m : compressed)
        context_.push(std::move(m));
    emit_context_event(context_events_, context_);

    // The cached server prompt count describes the PRE-compression context;
    // reset it so the gate evaluates the new context honestly (falls back
    // to the estimate until the next chat refreshes it).
    cfg_.prompt_tokens_used = -1;

    // Apply memory/skill ops from the LLM classification response.
    apply_compression_result(cr);

    // Stats — captured BEFORE the snapshot was taken.
    r.messages_before = msgs_before;
    r.messages_after = context_.size();
    r.tokens_before = tokens_before;
    r.tokens_after = context_.token_count();

    // Populate segment counts from the classification response.
    for (const auto& seg : cr.segments) {
        switch (seg.tag) {
            case Classification::core:    ++r.core_count; break;
            case Classification::context: ++r.context_count; break;
            case Classification::prune:   ++r.prune_count; break;
        }
    }

    reporter.on_compress_done(r);
    if (out) *out = std::move(r);
    return true;
}

std::string Agent::confirm_turn(const std::string& candidate,
                                const std::vector<std::shared_ptr<Tool>>& tools) {
    Message done_msg;
    done_msg.role = "user";
    done_msg.content =
        "Are you finished? If you need more information or analysis, "
        "use tools now. Otherwise reply with \"done.\"";
    context_.push(std::move(done_msg));
    emit_context_event(context_events_, context_);

    Message check = chat_with_recovery(tools, "probe", /*display=*/false,
                                       /*strict=*/true);
    // Template-style tool calls arrive embedded in the content (e.g. ornith's
    // <tool_call><function=...><parameter=...>) — extract them so the probe
    // dispatches them like the main loop does.
    extract_embedded_tool_calls(check);
    json check_tool_calls = check.tool_calls;
    std::string check_content = check.content;
    context_.push(std::move(check));
    emit_context_event(context_events_, context_);

    if (!check_tool_calls.is_null() && !check_tool_calls.empty()) {
        bool any_ran = dispatch_tool_calls(check_tool_calls, cfg_, registry_,
                                           hooks_, log_, session_approved_,
                                           &policy_, &context_);
        if (!any_ran) {
            // Scan from the back for the last tool result; if it was denied
            // the loop is broken.
            const auto& all = context_.get_all();
            for (auto it = all.rbegin(); it != all.rend(); ++it) {
                if (it->role != "tool") continue;
                if (it->content.find("status=denied") != std::string::npos)
                    return candidate;
                break;
            }
        }
        return "";
    }

    auto is_confirmation = [](const std::string& s) -> bool {
        std::string flat;
        for (char c : s) {
            char lc = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
            if (lc != ' ' && lc != '.' && lc != '!' && lc != '\n' && lc != '\r')
                flat += lc;
        }
        return flat == "done" || flat == "yes" || flat == "ok" ||
               flat == "finished" || flat == "looksgood" ||
               flat == "complete" || flat == "alldone";
    };

    if (is_confirmation(check_content) || check_content.empty())
        return candidate;
    return check_content;
}

void Agent::log_and_push_user_prompt(const std::string& prompt) {
    if (!log_.enabled()) {
        // Default-on transcript: nothing the agent saw is ever lost, even
        // when the user never configured log_path. Per-session file under
        // the workspace's .amber dir; {ts} expands to the session id.
        std::string path = cfg_.log_path;
        if (path.empty()) path = Workspace::local_dir() + "/logs/{ts}.jsonl";
        log_.open(path);
    }
    log_.event("user", {{"content", prompt}, {"model", cfg_.model}});
    Message msg;
    msg.role = "user";
    msg.content = prompt;
    context_.push(std::move(msg));
    emit_context_event(context_events_, context_);
}

bool Agent::dispatch_with_loop_detection(
    const json& tool_calls, const std::string& content,
    FailStreak& fail_streak,
    int& loop_count, std::string& last_loop_key,
    int& tool_recovery_attempts, std::string& final_reply) {
    if (tool_calls.is_null() || tool_calls.empty())
        return false;

    if (hooks_.on_assistant && !content.empty())
        hooks_.on_assistant(content);
    if (hooks_.on_state) hooks_.on_state(RunState::Tooling);
    if (hooks_.on_debug)
        hooks_.on_debug("dispatching " + std::to_string(tool_calls.size()) +
                        " tool call(s)");

    bool ok = dispatch_tool_calls(tool_calls, cfg_, registry_,
                                  hooks_, log_, session_approved_,
                                  &policy_, &context_);

    if (cfg_.detection_loop) {
        std::string cur = fingerprint_tool_calls(tool_calls);
        if (!cur.empty() && cur == last_loop_key) ++loop_count;
        else { loop_count = 0; last_loop_key = cur; }
        if (loop_count >= 3) {
            if (hooks_.on_status)
                hooks_.on_status("loop detected: breaking tool loop");
            log_.event("error", {{"reason", "tool_loop_detected"}});
            final_reply = "[loop detected: the model repeated the same tool "
                          "call 3+ times. Rephrase or break down the task.]";
            return true;
        }
        int worst = fail_streak.update(tool_calls, ok);
        if (worst >= 3) {
            if (tool_recovery_attempts >= 1) {
                if (hooks_.on_status)
                    hooks_.on_status("tool recovery failed, stopping");
                log_.event("tool_recovery", {{"action", "hard_stop"}});
                final_reply = "[stopped: tool calls kept failing after recovery "
                              "steer; rephrase your request or run a simpler command]";
                return true;
            }
            inject_tool_recovery_steer(&context_, hooks_, log_);
            ++tool_recovery_attempts;
        }
    }
    return true;
}

bool Agent::detect_text_loop(const std::string& content, int& text_loop_count,
                              std::string& last_text, std::string& final_reply) {
    if (!cfg_.detection_loop) return false;
    if (content == last_text && !content.empty()) {
        ++text_loop_count;
        if (text_loop_count == 2) {
            Message steer;
            steer.role = "user";
            steer.content = "You are repeating the same response. "
                "If you are done, say \"done.\" If you need more "
                "information, use a tool. Do not repeat yourself.";
            context_.push(std::move(steer));
            emit_context_event(context_events_, context_);
            if (hooks_.on_status)
                hooks_.on_status("text loop: injected recovery steer");
        }
        if (text_loop_count >= 5) {
            if (hooks_.on_status)
                hooks_.on_status("agent looped beyond recovery, stopping");
            log_.event("error", {{"reason", "text_loop_unrecoverable"}});
            final_reply = "[loop detected: the model repeated itself "
                          "and did not recover. Please rephrase your request.]";
            if (hooks_.on_assistant) hooks_.on_assistant(final_reply);
            return true;
        }
    } else {
        text_loop_count = 0;
        last_text = content;
    }
    return false;
}

std::string Agent::try_confirm(const std::string& candidate,
                                const std::vector<std::shared_ptr<Tool>>& tools) {
    std::string accepted = confirm_turn(candidate, tools);
    if (accepted.empty()) return {};
    if (hooks_.on_assistant) hooks_.on_assistant(accepted);
    log_.event("assistant", {{"content", accepted}});
    return accepted;
}

std::vector<std::shared_ptr<Tool>> Agent::resolve_tools() {
    // Keep the snapshot's ownership leases: raw pointers into a temporary
    // snapshot would dangle the moment the registry mutates.
    return registry_.snapshot_tools();
}

// One LLM round-trip with graceful recovery for request-side 4xx failures:
// the retry loop can REPAIR the request once (drop tools when the server
// cannot parse the tool grammar for the loaded template; swap to a
// server-known model id when the configured one is rejected) and retry.
// `display` controls whether the exchange paints into the scrollback;
// `strict` makes internal exchanges rethrow instead of faking a reply.
Message Agent::chat_with_recovery(const std::vector<std::shared_ptr<Tool>>& tools,
                                  const char* stage, bool display,
                                  bool strict) {
    auto chat = [this, &tools, display]() {
        return chat_once(tools, display);
    };
    auto chat_no_tools = [this, display]() {
        return chat_once({}, display);
    };
    ChatAdapter adapt =
        [this, &tools, &chat_no_tools, display](const std::string& err)
        -> std::function<Message()> {
        switch (classify_request_failure(err)) {
        case RequestFailure::TemplateParser:
            if (!tools.empty()) return chat_no_tools;
            break;
        case RequestFailure::ModelName: {
            auto models = list_models(cfg_);
            if (!models.empty() && models[0] != cfg_.model) {
                set_model(models[0]);
                if (hooks_.on_status)
                    hooks_.on_status("server rejected model \"" + cfg_.model +
                                     "\" - retrying with \"" + models[0] +
                                     "\"");
                return [this, &tools, display]() {
                    return chat_once(tools, display);
                };
            }
            break;
        }
        default:
            break;
        }
        return {};
    };
    if (strict)
        return chat_with_retry_strict(hooks_, log_, chat, stage,
                                      cfg_.cancel_token, 3, adapt);
    return chat_with_retry(hooks_, log_, chat, stage, cfg_.cancel_token, 3,
                           adapt);
}

// Cancellation path: returns a true empty reply (no empty-turn
// substitution — the stop was deliberate) and skips any further work.
std::string Agent::finish_turn_cancelled() {
    log_.event("turn_end", {{"reason", "cancelled"}});
    if (hooks_.on_state) hooks_.on_state(RunState::Idle);
    return "";
}

std::string Agent::finish_turn(std::string final_reply) {
    if (final_reply.empty()) {
        final_reply = empty_turn_reply(context_.get_all());
        log_.event("error", {{"reason", final_reply.find("tool calls") != std::string::npos
                                          ? "empty_after_tools" : "empty_reply"}});
    }
    log_.event("turn_end", {{"content", final_reply}});
    if (hooks_.on_state) hooks_.on_state(RunState::Idle);
    return final_reply;
}

std::string Agent::run(const std::string& user_prompt) {
    ensure_system_prompt();
    log_and_push_user_prompt(user_prompt);

    auto tools = resolve_tools();

    FailStreak fail_streak;
    int loop_count = 0, text_loop_count = 0, tool_recovery_attempts = 0;
    std::string last_loop_key, last_text, final_reply;
    const auto loop_t0 = std::chrono::steady_clock::now();
    const auto deadline = cfg_.max_wall_ms > 0
        ? loop_t0 + std::chrono::milliseconds(cfg_.max_wall_ms)
        : std::chrono::steady_clock::time_point::max();

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        // Wall-clock budget: the engine enforces max_wall_ms, not just the
        // post-hoc scorer — a runaway must stop at the deadline.
        if (std::chrono::steady_clock::now() >= deadline) {
            if (hooks_.on_status)
                hooks_.on_status("wall-clock budget exceeded, stopping");
            log_.event("error", {{"reason", "wall_clock_exceeded"}});
            final_reply =
                "[stopped: wall-clock budget exceeded; simplify the task "
                "or retry]";
            break;
        }
        // Cancellation ends the turn cleanly: no fabricated error message,
        // no probe round-trip, nothing pushed into the context.
        if (cfg_.cancel_token.is_requested()) {
            if (hooks_.on_status)
                hooks_.on_status("cancelled by user");
            return finish_turn_cancelled();
        }
        if (hooks_.on_debug)
            hooks_.on_debug("iteration " + std::to_string(iter + 1) + "/" +
                            std::to_string(cfg_.max_tool_iterations));
        Message reply;
        try {
            reply = chat_with_recovery(tools, "generation",
                                       /*display=*/true,
                                       /*strict=*/false);
        } catch (const CancelledError&) {
            if (hooks_.on_status)
                hooks_.on_status("cancelled by user");
            return finish_turn_cancelled();
        }

        // Extract tool_calls and content before push_reply (which moves reply).
        // Template-style tool calls arrive embedded in the content (e.g.
        // ornith's <tool_call><function=...><parameter=...>) — extract them
        // BEFORE the dispatch snapshot so they actually execute.
        extract_embedded_tool_calls(reply);
        json tc = reply.tool_calls;
        std::string content = reply.content;
        push_reply(std::move(reply));

        if (!tc.is_null() && !tc.empty() &&
            dispatch_with_loop_detection(tc, content, fail_streak, loop_count,
                                          last_loop_key, tool_recovery_attempts,
                                          final_reply)) {
            if (!final_reply.empty()) break;
            continue;
        }
        if (detect_text_loop(content, text_loop_count, last_text, final_reply))
            break;

        std::string accepted = try_confirm(content, tools);
        if (!accepted.empty()) { final_reply = accepted; break; }
    }
    return finish_turn(std::move(final_reply));
}

void Agent::apply_compression_result(const CompressionResponse& cr) {
    if (!memory_store_ || experience_cfg_.store_path.empty()) return;
    if (cr.memory_ops.empty() && cr.skill_ops.empty()) return;

    memory_store_->set_current_turn(turn_counter_);

    // Apply memory ops
    size_t mem_up = 0, mem_dep = 0;
    for (const auto& op : cr.memory_ops) {
        if (op.action == "deprecate") ++mem_dep; else ++mem_up;
    }
    std::vector<ExtractionItem> items;
    if (!cr.memory_ops.empty())
        apply_memory_ops(*memory_store_, cr.memory_ops,
                         experience_cfg_.store_path, &items);

    // Apply skill ops
    size_t sk_up = 0, sk_dep = 0;
    for (const auto& op : cr.skill_ops) {
        if (op.action == "deprecate") ++sk_dep; else ++sk_up;
    }
    if (!cr.skill_ops.empty())
        apply_skill_ops(*memory_store_, cr.skill_ops,
                        experience_cfg_.store_path, &items);

    // Decay and persist
    size_t before_decay = memory_store_->store_size();
    memory_store_->decay_all();
    size_t after_decay = memory_store_->store_size();
    if (!experience_cfg_.store_path.empty())
        memory_store_->save(experience_cfg_.store_path);
    if (hooks_.on_status) {
        size_t pruned = (before_decay > after_decay) ? before_decay - after_decay : 0;
        if (pruned > 0)
            hooks_.on_status("decay: " + std::to_string(pruned) + " items evicted ("
                             + std::to_string(before_decay) + " → "
                             + std::to_string(after_decay) + " total)");
    }

    // Log what happened
    auto log_status = [&](const std::string& msg) {
        if (hooks_.on_status) hooks_.on_status(msg);
    };
    std::string summary;
    if (mem_up) summary += std::to_string(mem_up) + " memories upserted";
    if (mem_dep) summary += (summary.empty() ? "" : ", ") + std::to_string(mem_dep) + " deprecated";
    if (sk_up) summary += (summary.empty() ? "" : ", ") + std::to_string(sk_up) + " skills upserted";
    if (sk_dep) summary += (summary.empty() ? "" : ", ") + std::to_string(sk_dep) + " deprecated";
    if (!summary.empty()) log_status("extraction: " + summary);
    size_t st = memory_store_->store_size();
    log_status("memory store: " + std::to_string(st) + " total (memories + skills)");
}

} // namespace agent
