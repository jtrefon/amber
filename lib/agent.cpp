// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/tool_call_parser.h"
#include "agent/agent_helpers.h"
#include "agent/tool_recovery.h"
#include "agent/dispatch.h"

#include <future>
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
    bool did_compress = false;
    if (gate_) {
        if (gate_->should_compress(history_, cfg_)) {
            auto cc = load_compression_config(cfg_);
            prompt_msgs = compression_->compress(prompt_msgs, cc);
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
    if (did_compress && memory_store_ && experience_cfg_.enabled) {
        std::thread([this]() {
            TreeShaker shaker;
            auto tags = shaker.classify(history_);
            size_t n = 0;
            for (size_t i = 0; i < history_.size() && i < tags.size(); ++i) {
                if (tags[i] != Classification::prune &&
                    history_[i].role == "tool" &&
                    history_[i].content.size() > 50) {
                    Memory mem;
                    auto nl = history_[i].content.find('\n');
                    mem.content = (nl == std::string::npos)
                        ? history_[i].content.substr(0, 200)
                        : history_[i].content.substr(0, nl);
                    mem.tags = {history_[i].name};
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

    auto cc = load_compression_config(cfg_);

    for (const auto& msg : history_)
        r.tokens_before += (msg.content.size() + msg.reasoning.size()) / 4;
    r.messages_before = history_.size();

    auto compressed = compression_->compress(history_, cc);
    if (!compressed.empty() && compressed.size() < history_.size()) {
        TreeShaker shaker;
        auto tags = shaker.classify(history_);
        for (auto t : tags) {
            if (t == Classification::core) ++r.core_count;
            else if (t == Classification::context) ++r.context_count;
            else if (t == Classification::prune) ++r.prune_count;
        }
        history_ = std::move(compressed);
    }

    for (const auto& msg : history_)
        r.tokens_after += (msg.content.size() + msg.reasoning.size()) / 4;
    r.messages_after = history_.size();

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
    // Loop detection: if the model makes the same tool calls more than
    // LOOP_REPEAT times without producing a text answer, break the loop.
    // This catches genuine loops (e.g. repeatedly reading the same file)
    // without penalizing thorough multi-step work.
    static constexpr int kLoopRepeat = 5;
    int loop_count = 0;
    json last_tool_calls;

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

            // Loop detection: identical tool call sets without progress
            if (ok && last_tool_calls == reply.tool_calls) {
                ++loop_count;
            } else {
                loop_count = 0;
                last_tool_calls = reply.tool_calls;
            }
            if (loop_count >= kLoopRepeat) {
                if (hooks_.on_status)
                    hooks_.on_status("loop detected: breaking tool loop");
                log_.event("error", {{"reason", "tool_loop_detected"}});
                break;
            }

            int worst = fail_streak.update(reply.tool_calls, ok);
            if (worst >= 3 &&
                inject_tool_recovery_steer(history_, hooks_, log_, final_reply))
                break;
            continue;
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
