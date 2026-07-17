// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/tool_call_parser.h"
#include <future>
#include <stdexcept>
#include <cctype>
#include <chrono>
#include <string>

namespace agent {

namespace {
// Parse one OpenAI tool_call delta into its id/name/args triple. The arguments
// field may arrive as a JSON string (streaming fragments) and is parsed here.
void parse_tool_call(const json& call, std::string& id, std::string& fn,
                     json& args) {
    auto str_or = [](const json& j, const char* k,
                     const std::string& d) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
    };
    json fnobj = call.contains("function") && call["function"].is_object()
                     ? call["function"] : json::object();
    id = str_or(call, "id", "");
    fn = str_or(fnobj, "name", "");
    args = fnobj.contains("arguments") && !fnobj["arguments"].is_null()
               ? fnobj["arguments"] : json::object();
    if (args.is_string()) {
        try { args = json::parse(args.get<std::string>()); }
        catch (...) { args = json::object(); }
    }
}
} // namespace

Agent::Agent(const Config& cfg, ToolRegistry& registry, AgentHooks hooks)
    : cfg_(cfg), registry_(registry), client_(cfg), hooks_(std::move(hooks)) {}

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

    // Mode-specific system instruction
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

bool Agent::approve_call(const Tool& tool, const json& args) {
    // Fail safe: with no approval handler, deny gated tools outright.
    if (!hooks_.on_approval) return false;
    std::string summary = tool.summarize(args);
    Approval d = hooks_.on_approval(tool.name(), args, summary);
    if (d == Approval::AllowSession) {
        session_approved_.insert(tool.name());
        return true;
    }
    return d == Approval::AllowOnce;
}

// Execute every requested tool call, recording results back into the
// conversation history so the model can consume them on the next iteration.
// Independent tools run in parallel via std::async; approval checks are
// synchronous to avoid race conditions on session_approved_.
void Agent::dispatch_tool_calls(const json& calls, std::vector<Tool*>&) {
    // Phase 1: parse + approval (sequential, no side effects yet).
    struct Call {
        std::string id, fn;
        json args;
        Tool* tool = nullptr;
        bool approved = false;
        std::string denied_reason;
    };
    std::vector<Call> todo;

    for (const auto& call : calls) {
        Call c;
        parse_tool_call(call, c.id, c.fn, c.args);
        if (hooks_.on_tool_call) hooks_.on_tool_call(c.fn, c.args);
        log_.event("tool_call", {{"name", c.fn}, {"id", c.id}, {"args", c.args}});

        c.tool = registry_.find(c.fn);
        if (!c.tool) {
            c.denied_reason = "unknown tool: " + c.fn;
        } else if (cfg_.mode == agent::AgentMode::Read && !c.tool->is_read_only()) {
            c.denied_reason = "tool \"" + c.fn + "\" is not available in read mode";
            log_.event("tool_denied", {{"name", c.fn}, {"id", c.id},
                                       {"reason", "read_mode"}});
        } else if (c.tool->requires_approval() &&
                   !session_approved_.count(c.fn) &&
                   cfg_.mode != agent::AgentMode::Yolo &&
                   !approve_call(*c.tool, c.args)) {
            c.denied_reason = "denied by user: " + c.fn + " was not approved";
            log_.event("tool_denied", {{"name", c.fn}, {"id", c.id},
                                       {"args", c.args}});
        } else {
            c.approved = true;
        }
        todo.push_back(std::move(c));
    }

    // Phase 2: execute approved tools in parallel.
    std::vector<std::future<ToolResult>> futures(todo.size());
    for (size_t i = 0; i < todo.size(); ++i) {
        if (!todo[i].approved) continue;
        futures[i] = std::async(std::launch::async, [&todo, i]() -> ToolResult {
            try { return todo[i].tool->execute(todo[i].args); }
            catch (const std::exception& e) {
                return ToolResult{false, "", std::string("tool threw: ") + e.what()};
            }
        });
    }

    // Phase 3: collect results in order.
    for (size_t i = 0; i < todo.size(); ++i) {
        auto& c = todo[i];
        ToolResult res;
        if (!c.approved) {
            res.ok = false;
            res.error = c.denied_reason;
        } else {
            res = futures[i].get();
        }

        if (hooks_.on_tool_result) hooks_.on_tool_result(c.fn, res);
        log_.event("tool_result", {{"name", c.fn}, {"id", c.id},
                                    {"ok", res.ok},
                                    {"output", res.ok ? res.output : res.error}});

        Message tool_msg;
        tool_msg.role = "tool";
        tool_msg.tool_call_id = c.id;
        tool_msg.name = c.fn;
        tool_msg.content = res.ok ? res.output : ("ERROR: " + res.error);
        history_.push_back(tool_msg);
    }
}

Message Agent::chat_once(std::vector<Tool*>& tools) {
    Message reply;
    Stats stats;
    if (hooks_.on_state) hooks_.on_state(RunState::Waiting);
    if (cfg_.stream) {
        reply = client_.chat_stream(history_, tools,
            [this](const StreamChunk& ch) {
                if (ch.done) return;
                if (!ch.reasoning.empty()) {
                    if (hooks_.on_state) hooks_.on_state(RunState::Thinking);
                    if (hooks_.on_reasoning) hooks_.on_reasoning(ch.reasoning);
                }
                if (!ch.delta.empty()) {
                    if (hooks_.on_state) hooks_.on_state(RunState::Streaming);
                    if (hooks_.on_token) hooks_.on_token(ch.delta);
                }
            }, &stats);
    } else {
        reply = client_.chat(history_, tools, &stats);
    }
    if (stats.valid && hooks_.on_stats) hooks_.on_stats(stats);
    return reply;
}

void Agent::reset() {
    history_.clear();
    session_approved_.clear();
}

std::string Agent::run(const std::string& user_prompt) {
    // Retain prior turns (stateful). Seed the system prompt only if this is a
    // brand-new conversation.
    ensure_system_prompt();
    if (!log_.enabled()) log_.open(cfg_.log_path);
    log_.event("user", {{"content", user_prompt}, {"model", cfg_.model}});

    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_prompt;
    history_.push_back(user_msg);

    std::string final_reply;
    auto set_state = [this](RunState s) { if (hooks_.on_state) hooks_.on_state(s); };

    std::vector<Tool*> tools;
    for (const auto& t : registry_.tools()) tools.push_back(t.get());

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        Message reply = chat_once(tools);
        history_.push_back(reply);
        if (!reply.reasoning.empty())
            log_.event("reasoning", {{"content", reply.reasoning}});

        // If the response has no JSON tool_calls but contains XML-embedded
        // tool calls (common with Jinja chat templates), extract them.
        bool has_json_tc = !reply.tool_calls.is_null() &&
                           !reply.tool_calls.empty();
        if (!has_json_tc && !reply.content.empty()) {
            auto extracted = extract_tool_calls_from_text(reply.content);
            if (!extracted.is_null()) {
                reply.tool_calls = std::move(extracted);
                // Remove the XML block from the stored content so it
                // doesn't leak into the visible conversation.
                reply.content.clear();
                history_.back().content.clear();
                history_.back().tool_calls = reply.tool_calls;
                has_json_tc = true;
                if (hooks_.on_status)
                    hooks_.on_status("parsed tool calls from text");
            }
        }


        // Dispatch tool calls whenever present — the model often narrates
        // ("I'll read that file") alongside the actual call. Dispatched
        // regardless of text content so multi-step chains work.
        if (!reply.tool_calls.is_null() && !reply.tool_calls.empty()) {
            set_state(RunState::Tooling);
            if (hooks_.on_status) hooks_.on_status("assistant requested tools");
            dispatch_tool_calls(reply.tool_calls, tools);
            continue;
        }

        // Plain-text reply: candidate answer. Before accepting it, ask the
        // model to validate — it may realise it needs more tools or want to
        // produce a better final summary. This replaces the old mode-gated
        // evaluator-optimizer with a universal loop-termination check.
        //   • model calls tools → main loop continues
        //   • model confirms    → candidate is accepted
        //   • model improves    → improved text becomes final
        std::string candidate = reply.content;

        Message done_msg;
        done_msg.role = "user";
        done_msg.content =
            "Are you finished? If you need more information or analysis, "
            "use tools now. Otherwise reply with \"done.\"";
        history_.push_back(done_msg);

        Message check = chat_once(tools);
        history_.push_back(check);

        if (!check.tool_calls.is_null() && !check.tool_calls.empty()) {
            // Model chose to do more work — continue the main loop.
            if (hooks_.on_status) hooks_.on_status("continuing investigation");
            dispatch_tool_calls(check.tool_calls, tools);
            continue;
        }

        // Model confirmed or improved the answer.
        // Normalise and check for a simple confirmation.
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

        if (is_confirmation(check.content) || check.content.empty()) {
            // Accept the candidate.
            final_reply = candidate;
        } else {
            // Model provided an improved answer — use it.
            final_reply = check.content;
        }

        if (hooks_.on_assistant) hooks_.on_assistant(final_reply);
        log_.event("assistant", {{"content", final_reply}});
        break;
    }


    if (final_reply.empty()) {
        final_reply = "[agent stopped: maximum tool iterations reached]";
        log_.event("error", {{"reason", "max_tool_iterations"}});
    }
    log_.event("turn_end", {{"content", final_reply}});
    set_state(RunState::Idle);
    return final_reply;
}

} // namespace agent
