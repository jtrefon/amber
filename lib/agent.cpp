// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/tool_call_parser.h"
#include "agent/agent_helpers.h"
#include "agent/tool_recovery.h"
#include "agent/dispatch.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace agent {

Agent::Agent(const Config& cfg, ToolRegistry& registry, AgentHooks hooks,
             std::unique_ptr<CompressionStrategy> compressor,
             std::unique_ptr<CompressionGate> gate,
             std::unique_ptr<MemoryStore> memory_store,
             std::unique_ptr<MemoryRetriever> retriever)
    : cfg_(cfg), registry_(registry), client_(cfg), hooks_(std::move(hooks))
    , compression_(std::move(compressor))
    , gate_(std::move(gate))
    , memory_store_(std::move(memory_store))
    , retriever_(std::move(retriever)) {
    experience_cfg_ = load_experience_config(cfg_);
}

void Agent::ensure_system_prompt() {
    if (!history_.empty() && history_.front().role == "system") return;

    std::string system = load_prompt(cfg_.system_prompt_path);
    if (system.empty())
        system = "You are amber, a helpful coding assistant running on a "
                 "Linux server. Use the bash tool for ALL file operations. "
                 "Read files with: cat, head, tail, grep, ls. "
                 "Edit files with: sed, redirect (>), or use the write tool. "
                 "Run builds with: g++, make, cmake. "
                 "Read-only commands (cat, ls, grep, find) need no approval. "
                 "Write commands (sed -i, rm, mv, >) may be gated by policy.";
    if (!cfg_.tools_prompt_path.empty())
        system += "\n\n" + load_prompt(cfg_.tools_prompt_path);
    else if (!registry_.empty())
        system += "\n\n" + render_tools_markdown(registry_);

    switch (cfg_.mode) {
    case agent::AgentMode::Read:
        system += "\n\nYou are in READ mode. You can only use read-only tools "
                  "(search, grep, read). Writing files or running shell commands "
                  "is disallowed. Answer questions about the codebase but do not "
                  "make changes.";
        break;
    case agent::AgentMode::Yolo:
        system += "\n\nYou are in YOLO mode. All tools are auto-approved and "
                  "execute immediately. Be thorough but efficient; the user trusts "
                  "you to make the right decisions.";
        break;
    case agent::AgentMode::Write:
    default:
        break;
    }

    Message sys_msg;
    sys_msg.role = "system";
    sys_msg.content = system;
    history_.insert(history_.begin(), sys_msg);
}

void Agent::set_history(std::vector<Message> messages) {
    history_ = std::move(messages);
}

Message Agent::chat_once(const std::vector<Tool*>& tools, bool display) {
    Message reply;
    Stats stats;
    if (hooks_.on_debug) hooks_.on_debug("chat: request");
    if (hooks_.on_state) hooks_.on_state(RunState::Waiting);

    // Build the prompt: start with a copy of history so we can augment without
    // modifying the stored conversation.
    auto prompt_msgs = history_;

    // Inject memories into the system message (copy only, not stored history).
    if (retriever_) {
        std::string user_msg;
        for (const auto& m : prompt_msgs)
            if (m.role == "user") { user_msg = m.content; break; }
        auto suffix = retriever_->build_system_prompt_suffix(user_msg, 500);
        if (!suffix.empty()) {
            for (auto& msg : prompt_msgs) {
                if (msg.role == "system") {
                    msg.content += suffix;
                    break;
                }
            }
        }
    }

    // Check compression gate.  If triggered, compress non-system messages.
    if (gate_ && compression_) {
        if (gate_->should_compress(history_, cfg_)) {
            auto cc = load_compression_config(cfg_);
            prompt_msgs = compression_->compress(prompt_msgs, cc, client_);
            gate_->set_last_compress_turn(turn_counter_);
        }
    }

    // The internal confirmation exchange (confirm_turn) must not paint tokens
    // into the scrollback — otherwise the model's literal "done." leaks into
    // the displayed conversation. We keep state transitions so the UI stays
    // honest about activity, but drop token/reasoning/assistant callbacks.
    const AgentHooks& h = display ? hooks_ : silent_hooks();
    if (cfg_.stream) {
        reply = client_.chat_stream(prompt_msgs, tools,
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
        reply = client_.chat(prompt_msgs, tools, &stats);
    }
    if (stats.valid && hooks_.on_stats) hooks_.on_stats(stats);

    ++turn_counter_;
    return reply;
}

// Hooks with display callbacks nulled out, used for the silent confirmation
// exchange so its tokens never reach the scrollback.
const AgentHooks& Agent::silent_hooks() const {
    static const AgentHooks silent = [] {
        AgentHooks h;
        h.on_state = [](RunState) {};
        return h;
    }();
    return silent;
}

namespace {

size_t estimate_tokens(const std::vector<Message>& msgs) {
    size_t n = 0;
    for (const auto& m : msgs)
        n += (m.content.size() + m.reasoning.size()) / 4;
    return n;
}

void apply_compression_memops(MemoryStore* store, const ExperienceConfig& exp_cfg,
                               const CompressionResponse& cr, size_t turn,
                               CompressionReporter& reporter) {
    size_t upsert = 0, deprecate = 0;
    for (const auto& op : cr.memory_ops) {
        if (op.action == "deprecate") ++deprecate; else ++upsert;
    }
    if (!store || cr.memory_ops.empty()) return;
    store->set_current_turn(turn);
    apply_memory_ops(*store, cr.memory_ops, "");
    store->decay_all();
    if (!exp_cfg.store_path.empty())
        store->save(exp_cfg.store_path);
    reporter.on_memory_ops_applied(upsert, deprecate);
}

} // namespace

CompressionResult Agent::compress_now() {
    CompressionResult r;
    if (!compression_ || history_.size() < 2) return r;
    auto before = history_;

    CompressionReporter reporter(hooks_, r);
    reporter.set_before(before.size(), estimate_tokens(before));

    CompressionConfig cc = load_compression_config(cfg_);
    CompressionResponse pipeline_cr;
    history_ = compression_->compress(before, cc, client_, &reporter, &pipeline_cr);

    r.tokens_after = estimate_tokens(history_);
    r.messages_after = history_.size();
    r.core_count = r.messages_after;

    apply_compression_memops(memory_store_.get(), experience_cfg_,
                             pipeline_cr, turn_counter_, reporter);
    last_compression_ = r;
    return r;
}

void Agent::reset() {
    history_.clear();
    session_approved_.clear();
}

// Accept the candidate when the model confirms (or improves) the answer, or
// continue the loop when it asks for more tools. Returns the final reply, or
// an empty string meaning "keep iterating".
std::string Agent::confirm_turn(const std::string& candidate,
                                const std::vector<Tool*>& tools) {
    Message done_msg;
    done_msg.role = "user";
    done_msg.content =
        "Are you finished? If you need more information or analysis, "
        "use tools now. Otherwise reply with \"done.\"";
    history_.push_back(done_msg);

    Message check = chat_once(tools, /*display=*/false);
    history_.push_back(check);
    if (!check.tool_calls.is_null() && !check.tool_calls.empty()) {
        if (hooks_.on_status) hooks_.on_status("continuing investigation");
        dispatch_tool_calls(check.tool_calls, cfg_, registry_, hooks_, log_,
                            session_approved_, history_);
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

    if (is_confirmation(check.content) || check.content.empty())
        return candidate;
    return check.content;
}

void Agent::log_and_push_user_prompt(const std::string& prompt) {
    if (!log_.enabled()) log_.open(cfg_.log_path);
    log_.event("user", {{"content", prompt}, {"model", cfg_.model}});
    Message msg;
    msg.role = "user";
    msg.content = prompt;
    history_.push_back(msg);
}

bool Agent::dispatch_with_loop_detection(
    const Message& reply, FailStreak& fail_streak,
    int& loop_count, std::string& last_loop_key,
    int& tool_recovery_attempts, std::string& final_reply) {
    if (reply.tool_calls.is_null() || reply.tool_calls.empty())
        return false;

    if (hooks_.on_assistant && !reply.content.empty())
        hooks_.on_assistant(reply.content);
    if (hooks_.on_state) hooks_.on_state(RunState::Tooling);
    if (hooks_.on_status) hooks_.on_status("assistant requested tools");
    if (hooks_.on_debug)
        hooks_.on_debug("dispatching " + std::to_string(reply.tool_calls.size()) +
                        " tool call(s)");

    bool ok = dispatch_tool_calls(reply.tool_calls, cfg_, registry_,
                                  hooks_, log_, session_approved_, history_);

    if (cfg_.detection_loop) {
        auto loop_key = [](const json& calls) -> std::string {
            std::string key;
            for (const auto& tc : calls) {
                auto fn = tc.value("function", json::object());
                key += fn.value("name", "") + ":" + fn.value("arguments", "") + "|";
            }
            return key;
        };
        if (ok) {
            std::string cur = loop_key(reply.tool_calls);
            if (cur == last_loop_key) ++loop_count;
            else { loop_count = 0; last_loop_key = cur; }
        }
        if (loop_count >= 3) {
            if (hooks_.on_status)
                hooks_.on_status("loop detected: breaking tool loop");
            log_.event("error", {{"reason", "tool_loop_detected"}});
            return true;
        }
        int worst = fail_streak.update(reply.tool_calls, ok);
        if (worst >= 3) {
            if (tool_recovery_attempts >= 1) {
                if (hooks_.on_status)
                    hooks_.on_status("tool recovery failed, stopping");
                log_.event("tool_recovery", {{"action", "hard_stop"}});
                final_reply = "[stopped: tool calls kept failing after recovery "
                              "steer; rephrase your request or run a simpler command]";
                return true;
            }
            inject_tool_recovery_steer(history_, hooks_, log_);
            ++tool_recovery_attempts;
        }
    }
    return true;
}

bool Agent::detect_text_loop(const Message& reply, int& text_loop_count,
                              std::string& last_text, std::string& final_reply) {
    if (!cfg_.detection_loop) return false;
    if (reply.content == last_text && !reply.content.empty()) {
        ++text_loop_count;
        if (text_loop_count == 2) {
            Message steer;
            steer.role = "user";
            steer.content = "You are repeating the same response. "
                "If you are done, say \"done.\" If you need more "
                "information, use a tool. Do not repeat yourself.";
            history_.push_back(steer);
            if (hooks_.on_status)
                hooks_.on_status("text loop: injected recovery steer");
            last_text.clear();
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
        last_text = reply.content;
    }
    return false;
}

std::string Agent::try_confirm(const std::string& candidate,
                                const std::vector<Tool*>& tools) {
    std::string accepted = confirm_turn(candidate, tools);
    if (accepted.empty()) return {};
    if (hooks_.on_assistant) hooks_.on_assistant(accepted);
    log_.event("assistant", {{"content", accepted}});
    return accepted;
}

std::vector<Tool*> Agent::resolve_tools() {
    std::vector<Tool*> tools;
    for (const auto& t : registry_.tools()) tools.push_back(t.get());
    return tools;
}

std::function<Message()> Agent::make_chat_fn(const std::vector<Tool*>& tools) {
    return [this, &tools]() { return chat_once(tools); };
}

void Agent::process_generation(Message& reply) {
    history_.push_back(reply);
    if (!reply.reasoning.empty())
        log_.event("reasoning", {{"content", reply.reasoning}});
    maybe_extract_text_tool_calls(reply.tool_calls, reply.content,
                                  history_.back(), hooks_);
}

std::string Agent::finish_turn(std::string final_reply) {
    if (final_reply.empty()) {
        final_reply = empty_turn_reply(history_);
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
    auto chat = make_chat_fn(tools);

    FailStreak fail_streak;
    int loop_count = 0, text_loop_count = 0, tool_recovery_attempts = 0;
    std::string last_loop_key, last_text, final_reply;

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        if (hooks_.on_debug)
            hooks_.on_debug("iteration " + std::to_string(iter + 1) + "/" +
                            std::to_string(cfg_.max_tool_iterations));
        Message reply = safe_chat_once(hooks_, log_, chat, "generation");
        process_generation(reply);

        if (dispatch_with_loop_detection(reply, fail_streak, loop_count,
                                          last_loop_key, tool_recovery_attempts,
                                          final_reply)) {
            if (!final_reply.empty()) break;
            continue;
        }
        if (detect_text_loop(reply, text_loop_count, last_text, final_reply))
            break;

        std::string accepted = try_confirm(reply.content, tools);
        if (!accepted.empty()) { final_reply = accepted; break; }
    }
    return finish_turn(std::move(final_reply));
}

} // namespace agent
