// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/tool_call_parser.h"
#include "agent/agent_helpers.h"
#include "agent/tool_recovery.h"
#include "agent/dispatch.h"

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <unistd.h>

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
    bool did_compress = false;
    if (gate_ && compression_) {
        if (gate_->should_compress(history_, cfg_)) {
            auto cc = load_compression_config(cfg_);
            prompt_msgs = compression_->compress(prompt_msgs, cc, client_);
            gate_->set_last_compress_turn(turn_counter_);
            did_compress = true;
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

    // If compression ran, fire async experience extraction.
    // Uses simple heuristics on tool results from the full (uncompressed)
    // history.  Full LLM-based extraction happens in compress_now().
    if (did_compress && memory_store_ && experience_cfg_.enabled) {
        std::thread([this]() {
            size_t n = 0;
            for (const auto& msg : history_) {
                if (msg.role == "tool" && msg.content.size() > 50 &&
                    msg.content.size() < 5000) {
                    Memory mem;
                    auto nl = msg.content.find('\n');
                    mem.content = (nl == std::string::npos)
                        ? msg.content.substr(0, 200)
                        : msg.content.substr(0, nl);
                    mem.tags = {msg.name};
                    mem.evidence_count = 1;
                    memory_store_->upsert(mem);
                    ++n;
                }
            }
            memory_store_->decay_all();
            if (!experience_cfg_.store_path.empty())
                memory_store_->save(experience_cfg_.store_path);
            last_extraction_.new_memories += n;
        }).detach();
    }

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

CompressionResult Agent::compress_now() {
    CompressionResult r;
    if (!compression_) return r;
    if (history_.size() < 2) return r;    // nothing meaningful to compress

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
    };
    auto status = [&](const std::string& msg) {
        std::string line = "[+" + std::to_string(elapsed_ms() / 1000) + "s] " + msg + "\n";
        if (hooks_.on_status) hooks_.on_status(line);
        // Unbuffered write — stderr is visible even when the TUI event loop
        // is blocked by this synchronous call.
        write(STDERR_FILENO, line.c_str(), line.size());
    };

    // Snapshot state before any mutation.
    auto before = history_;
    for (const auto& msg : before)
        r.tokens_before += (msg.content.size() + msg.reasoning.size()) / 4;
    r.messages_before = before.size();

    status("compress started  (" + std::to_string(r.messages_before) +
           " msgs, ~" + std::to_string(r.tokens_before) + " tokens)");

    // Step 1: collapse detected loops (modifies history_ in place).
    size_t pre_loop = history_.size();
    collapse_loops(history_);
    size_t loops_found = pre_loop - history_.size();
    if (loops_found > 0)
        status("loop collapse: removed " + std::to_string(loops_found) +
               " repeated messages");

    // Step 2: build and append the compression request.
    Message req = build_compression_request(history_);
    history_.push_back(req);

    status("LLM request sent — waiting for classification...");

    // Step 3: one LLM call — same system prompt, no tools.
    Message reply;
    try {
        reply = client_.chat(history_, {});
    } catch (const std::exception& e) {
        history_ = before;
        status("FAILED — " + std::string(e.what()));
        return r;
    }
    history_.pop_back();

    status("LLM replied (" + std::to_string(elapsed_ms() / 1000) + "s)"
           "  — parsing response...");

    // Step 4: parse the LLM response.
    auto cr = parse_compression_response(reply.content);
    if (cr.segments.empty()) {
        history_ = before;
        status("FAILED — unparseable LLM response, compression aborted");
        return r;
    }

    status("parsed " + std::to_string(cr.segments.size()) + " classification spans, "
           + std::to_string(cr.memory_ops.size()) + " memory ops, "
           + std::to_string(cr.skill_ops.size()) + " skill ops");

    // Build per-turn tags for statistics (based on pre-collapse indexing).
    std::vector<Classification> per_turn_tags(before.size(), Classification::core);
    if (before.size() > 0) {
        for (const auto& seg : cr.segments) {
            size_t end = std::min(seg.turn_end, before.size() - 1);
            for (size_t i = seg.turn_start; i <= end && i < before.size(); ++i)
                per_turn_tags[i] = seg.tag;
        }
    }

    // Step 5: apply classification to produce compressed history.
    history_ = apply_classification(history_, cr);

    for (const auto& msg : history_)
        r.tokens_after += (msg.content.size() + msg.reasoning.size()) / 4;
    r.messages_after = history_.size();
    for (auto t : per_turn_tags) {
        if (t == Classification::core) ++r.core_count;
        else if (t == Classification::context) ++r.context_count;
        else if (t == Classification::prune) ++r.prune_count;
    }

    status("apply: " + std::to_string(r.core_count) + " core kept, "
           + std::to_string(r.context_count) + " archived, "
           + std::to_string(r.prune_count) + " pruned"
           "  (" + std::to_string(r.messages_before) + " → "
           + std::to_string(r.messages_after) + " msgs, "
           + std::to_string(r.tokens_before) + " → "
           + std::to_string(r.tokens_after) + " tokens)");

    // Step 6: apply memory/skill upsert/deprecate ops from the LLM.
    size_t mem_upsert = 0, mem_deprecate = 0;
    size_t sk_upsert = 0, sk_deprecate = 0;
    for (const auto& op : cr.memory_ops) {
        if (op.action == "deprecate") ++mem_deprecate;
        else ++mem_upsert;
    }
    for (const auto& op : cr.skill_ops) {
        if (op.action == "deprecate") ++sk_deprecate;
        else ++sk_upsert;
    }

    if (memory_store_ && (!cr.memory_ops.empty() || !cr.skill_ops.empty())) {
        memory_store_->set_current_turn(turn_counter_);

        status("store: " + std::to_string(mem_upsert) + " memory upserts, "
               + std::to_string(mem_deprecate) + " deprecations, "
               + std::to_string(sk_upsert) + " skill upserts, "
               + std::to_string(sk_deprecate) + " deprecations");

        // List each memory op with preview
        for (const auto& op : cr.memory_ops) {
            std::string preview = op.content;
            if (preview.size() > 80) { preview.resize(77); preview += "..."; }
            status("  memory " + op.action + ": " + preview);
        }
        for (const auto& op : cr.skill_ops) {
            std::string preview = op.content;
            if (preview.size() > 80) { preview.resize(77); preview += "..."; }
            status("  skill " + op.action + ": " + preview);
        }

        apply_memory_ops(*memory_store_, cr.memory_ops, "");
        apply_skill_ops(*memory_store_, cr.skill_ops, "");

        status("store: applying decay...");
        size_t mem_before = memory_store_->top_memories(1000, "").size();
        memory_store_->decay_all();
        size_t mem_after = memory_store_->top_memories(1000, "").size();
        size_t decayed = (mem_before > mem_after) ? (mem_before - mem_after) : 0;
        if (decayed > 0 || mem_before > 0)
            status("store: decay removed " + std::to_string(decayed) + " items"
                   "  (was " + std::to_string(mem_before)
                   + ", now " + std::to_string(mem_after) + ")");

        if (!experience_cfg_.store_path.empty()) {
            status("store: saving to " + experience_cfg_.store_path + "...");
            memory_store_->save(experience_cfg_.store_path);
        }
        status("store: done");
    }

    long long total_ms = elapsed_ms();
    status("finished in " + std::to_string(total_ms / 1000) + "s ("
           + std::to_string(total_ms) + "ms)"
           "  " + std::to_string(r.messages_before) + " → "
           + std::to_string(r.messages_after) + " msgs, ~"
           + std::to_string(r.tokens_before) + " → ~"
           + std::to_string(r.tokens_after) + " tokens");

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

std::string Agent::run(const std::string& user_prompt) {
    ensure_system_prompt();
    if (!log_.enabled()) log_.open(cfg_.log_path);
    log_.event("user", {{"content", user_prompt}, {"model", cfg_.model}});

    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_prompt;
    history_.push_back(user_msg);
    std::string final_reply;
    auto dbg = [this](const std::string& msg) {
        if (hooks_.on_debug) hooks_.on_debug(msg);
    };
    auto set_state = [this](RunState s) { if (hooks_.on_state) hooks_.on_state(s); };
    std::vector<Tool*> tools;
    for (const auto& t : registry_.tools()) tools.push_back(t.get());
    auto chat = [this, &tools]() -> Message { return chat_once(tools); };

    FailStreak fail_streak;
    int tool_recovery_attempts = 0;
    // Loop detection: if the model makes the same tool calls more than
    // LOOP_REPEAT times without producing a text answer, break the loop.
    // This catches genuine loops (e.g. repeatedly reading the same file)
    // without penalizing thorough multi-step work.  Can be disabled at
    // runtime via cfg_.detection_loop (/set loop detection off).
    static constexpr int kLoopRepeat = 3;
    int loop_count = 0;
    std::string last_loop_key;
    int text_loop_count = 0;
    std::string last_text;

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        dbg("iteration " + std::to_string(iter + 1) + "/" +
            std::to_string(cfg_.max_tool_iterations));
        Message reply = safe_chat_once(hooks_, log_, chat, "generation");
        history_.push_back(reply);
        if (!reply.reasoning.empty())
            log_.event("reasoning", {{"content", reply.reasoning}});

        maybe_extract_text_tool_calls(reply.tool_calls, reply.content,
                                      history_.back(), hooks_);

        if (!reply.tool_calls.is_null() && !reply.tool_calls.empty()) {
            if (hooks_.on_assistant && !reply.content.empty())
                hooks_.on_assistant(reply.content);
            set_state(RunState::Tooling);
            if (hooks_.on_status) hooks_.on_status("assistant requested tools");
            dbg("dispatching " + std::to_string(reply.tool_calls.size()) +
                " tool call(s)");
            bool ok = dispatch_tool_calls(reply.tool_calls, cfg_, registry_,
                                          hooks_, log_, session_approved_,
                                          history_);

            // Loop detection: same tool names + same arguments repeated.
            // Disabled when cfg_.detection_loop is false (/set loop detection off).
            if (cfg_.detection_loop) {
                auto loop_key = [](const json& calls) -> std::string {
                    std::string key;
                    for (const auto& tc : calls) {
                        auto fn = tc.value("function", json::object());
                        key += fn.value("name", "") + ":";
                        key += fn.value("arguments", "") + "|";
                    }
                    return key;
                };
                if (ok) {
                    std::string cur = loop_key(reply.tool_calls);
                    if (cur == last_loop_key) {
                        ++loop_count;
                    } else {
                        loop_count = 0;
                        last_loop_key = cur;
                    }
                }
                if (loop_count >= kLoopRepeat) {
                    if (hooks_.on_status)
                        hooks_.on_status("loop detected: breaking tool loop");
                    log_.event("error", {{"reason", "tool_loop_detected"}});
                    break;
                }

                int worst = fail_streak.update(reply.tool_calls, ok);
                if (worst >= 3) {
                    if (tool_recovery_attempts >= 1) {
                        // Second recovery cycle with no improvement — hard stop
                        if (hooks_.on_status)
                            hooks_.on_status("tool recovery failed, stopping");
                        log_.event("tool_recovery", {{"action", "hard_stop"}});
                        final_reply =
                            "[stopped: tool calls kept failing after recovery "
                            "steer; rephrase your request or run a simpler command]";
                        break;
                    }
                    inject_tool_recovery_steer(history_, hooks_, log_);
                    ++tool_recovery_attempts;
                    continue;
                }
            }
            continue;
        }

        // Text loop detection.
        // Disabled when cfg_.detection_loop is false (/set loop detection off).
        if (cfg_.detection_loop) {
            // Phase 1 (count=2): inject a recovery steer asking the model to
            //   move on or use a tool.  Does not break the loop.
            // Phase 2 (count=5): hard break — model looped beyond recovery.
            if (reply.content == last_text && !reply.content.empty()) {
                ++text_loop_count;
                if (text_loop_count == 2) {
                    // Phase 1: soft recovery
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
                    // Phase 2: hard break
                    if (hooks_.on_status)
                        hooks_.on_status("agent looped beyond recovery, stopping");
                    log_.event("error", {{"reason", "text_loop_unrecoverable"}});
                    final_reply = "[loop detected: the model repeated itself "
                                 "and did not recover. Please rephrase your request.]";
                    if (hooks_.on_assistant) hooks_.on_assistant(final_reply);
                    break;
                }
            } else {
                text_loop_count = 0;
                last_text = reply.content;
            }
        }

        std::string candidate = reply.content;
        std::string accepted = confirm_turn(candidate, tools);
        if (!accepted.empty()) {
            final_reply = accepted;
            if (hooks_.on_assistant) hooks_.on_assistant(final_reply);
            log_.event("assistant", {{"content", final_reply}});
            break;
        }
    }

    if (final_reply.empty()) {
        final_reply = empty_turn_reply(history_);
        log_.event("error", {{"reason", final_reply.find("tool calls") != std::string::npos
                                          ? "empty_after_tools" : "empty_reply"}});
    }
    log_.event("turn_end", {{"content", final_reply}});
    set_state(RunState::Idle);
    return final_reply;
}

} // namespace agent
