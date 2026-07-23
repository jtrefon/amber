// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/dispatch.h"
#include "agent/agent_helpers.h"

#include <future>

namespace agent {

namespace {

// Check if a tool call (name + arguments) already exists in history.
// Returns a descriptive message string if duplicate, empty string if not.
std::string find_duplicate_call(const std::string& fn, const json& args,
                                 const std::vector<Message>& history) {
    for (const auto& m : history) {
        if (m.role == "assistant" && !m.tool_calls.is_null()) {
            for (const auto& tc : m.tool_calls) {
                auto func = tc.value("function", json::object());
                if (func.value("name", "") != fn) continue;
                // Stored arguments is a JSON-encoded string (from the LLM);
                // compare by parsing both to eliminate whitespace differences.
                json stored_args;
                try {
                    stored_args = json::parse(func.value("arguments", ""));
                } catch (...) {
                    continue;
                }
                // Compare parsed JSON objects (eliminates whitespace differences
                // between the LLM's raw arguments string and our re-serialization).
                if (!(stored_args == args)) continue;
                // Build a short summary of the arguments (first 120 chars)
                std::string preview;
                if (args.is_object()) {
                    for (auto it = args.begin(); it != args.end(); ++it) {
                        if (it.value().is_string())
                            preview += it.value().get<std::string>() + " ";
                        else
                            preview += it.key() + " ";
                    }
                }
                if (preview.size() > 120)
                    preview.resize(120);
                return "You already ran \"" + fn + "\" with these "
                       "exact parameters (" + preview + "...). "
                       "Repeating the same tool call will produce the "
                       "same result. Use the output already in the "
                       "conversation, or if you hit a loop, say \"done\" "
                        "or \"stop\" to end this task.";
            }
        }
    }
    return {};
}


} // namespace

// Fail-safe approval: with no handler, deny gated tools outright. A session
// grant is recorded so the same tool is auto-approved for the rest of the turn.
bool approve_tool(const Tool& tool, const json& args, const AgentHooks& hooks,
                  std::set<std::string>& session_approved) {
    if (!hooks.on_approval) return false;
    std::string summary = tool.summarize(args);
    Approval d = hooks.on_approval(tool.name(), args, summary);
    if (d == Approval::AllowSession) {
        session_approved.insert(tool.name());
        return true;
    }
    return d == Approval::AllowOnce;
}

bool dispatch_tool_calls(const json& calls, const Config& cfg,
                         ToolRegistry& registry, const AgentHooks& hooks,
                         ConversationLog& log,
                         std::set<std::string>& session_approved,
                         std::vector<Message>& history) {
    struct Call {
        std::string id, fn;
        json args;
        bool args_ok = true;
        Tool* tool = nullptr;
        bool approved = false;
        std::string denied_reason;
    };
    std::vector<Call> todo;

    for (const auto& call : calls) {
        Call c;
        parse_tool_call(call, c.id, c.fn, c.args, c.args_ok);
        if (hooks.on_tool_call) hooks.on_tool_call(c.fn, c.args);
        if (hooks.on_debug) hooks.on_debug("tool_call: " + c.fn);
        log.event("tool_call", {{"name", c.fn}, {"id", c.id}, {"args", c.args}});

        c.tool = registry.find(c.fn);
        if (!c.tool) {
            c.denied_reason = "unknown tool: " + c.fn;
        } else if (cfg.mode == agent::AgentMode::Read && !c.tool->is_read_only()) {
            c.denied_reason = "tool \"" + c.fn + "\" is not available in read mode";
            log.event("tool_denied", {{"name", c.fn}, {"id", c.id},
                                      {"reason", "read_mode"}});
        } else if (c.args_ok) {
            // Duplicate detection: skip when disabled (/set detection duplicate off).
            std::string dup;
            if (cfg.detection_duplicate)
                dup = find_duplicate_call(c.fn, c.args, history);
            if (!dup.empty()) {
                c.denied_reason = dup;
            } else if (c.tool->requires_approval() &&
                       !session_approved.count(c.fn) &&
                       cfg.mode != agent::AgentMode::Yolo &&
                       !(hooks.on_approval &&
                         approve_tool(*c.tool, c.args, hooks, session_approved))) {
                c.denied_reason = "denied by user: " + c.fn + " was not approved";
                log.event("tool_denied", {{"name", c.fn}, {"id", c.id},
                                          {"args", c.args}});
            } else {
                c.approved = true;
            }
        }
        todo.push_back(std::move(c));
    }

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

    bool all_ok = true;
    for (size_t i = 0; i < todo.size(); ++i) {
        auto& c = todo[i];
        ToolResult res;
        if (!c.approved) {
            res.ok = false;
            res.error = c.denied_reason;
        } else if (!c.args_ok) {
            res.ok = false;
            std::string raw = c.args.is_string() ? c.args.get<std::string>()
                                                  : c.args.dump();
            res.error = "tool call arguments were not valid JSON (truncated or "
                        "malformed): " + raw.substr(0, 200);
        } else {
            res = futures[i].get();
        }
        if (!res.ok) all_ok = false;

        if (hooks.on_tool_result) hooks.on_tool_result(c.fn, res);
        if (hooks.on_debug)
            hooks.on_debug("tool_result: " + c.fn + " (" +
                            (res.ok ? "ok" : "error") + ")");
        log.event("tool_result", {{"name", c.fn}, {"id", c.id},
                                    {"ok", res.ok},
                                    {"output", res.ok ? res.output : res.error}});

        Message tool_msg;
        tool_msg.role = "tool";
        tool_msg.tool_call_id = c.id;
        tool_msg.name = c.fn;
        // Set args_used so the envelope can echo them back to the model
        json call_args = c.args;
        // Ensure denied/error tools still get args echoed
        tool_msg.content = utf8_sanitize(
            format_tool_envelope(c.fn, call_args, res));
        history.push_back(tool_msg);
    }
    return all_ok;
}

} // namespace agent
