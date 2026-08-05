
#include "agent/dispatch.h"
#include "agent/agent_helpers.h"
#include "agent/context.h"
#include "agent/policy.h"

#include <chrono>
#include <future>
#include <thread>

namespace agent {

namespace {

// Build a canonical key for a tool call: "fn|args_dump".
// args is assumed to be already-parsed JSON (object).
std::string call_key(const std::string& fn, const json& args) {
    return fn + "|" + (args.is_object() ? args.dump() : json::object().dump());
}

// Look up the tool result for a given tool_call_id and extract its
// status from the envelope header (e.g. "ok", "error", "denied").
// Returns "unknown" if not found.
std::string prev_call_outcome(const std::string& tool_call_id,
                                const std::deque<Message>& history) {
    if (tool_call_id.empty()) return "unknown";
    for (const auto& m : history) {
        if (m.role != "tool") continue;
        if (m.tool_call_id != tool_call_id) continue;
        // Envelope: [tool=name args=... status=X meta=...]\n...
        auto pos = m.content.find("status=");
        if (pos == std::string::npos) return "unknown";
        pos += 7; // skip "status="
        auto end = m.content.find(' ', pos);
        if (end == std::string::npos) end = m.content.find(']', pos);
        if (end == std::string::npos) return "unknown";
        return m.content.substr(pos, end - pos);
    }
    return "unknown";
}

// Check if a previous tool call was denied (not executed) by looking for
// the corresponding tool result message in history. Returns true if the
// previous call matching the given `tool_call_id` was denied by policy.
bool was_prev_call_denied(const std::string& tool_call_id,
                            const std::deque<Message>& history) {
    if (tool_call_id.empty()) return false;
    return prev_call_outcome(tool_call_id, history) == "denied";
}

// Check if a tool call (name + arguments) already exists in a PRIOR
// assistant message — one whose tool_calls do NOT include the current
// call's `id`.  This prevents a batch of calls in the current turn from
// matching each other while still catching genuine repeats across turns.
// Calls that were previously DENIED (not executed) are skipped, allowing
// retries after the user extends permissions.
// Returns a descriptive message string if duplicate, empty string if not.
std::string find_duplicate_call(const std::string& fn, const json& args,
                                  const std::deque<Message>& history,
                                 const std::string& current_id) {
    std::string needle = call_key(fn, args);
    for (const auto& m : history) {
        if (m.role != "assistant" || m.tool_calls.is_null()) continue;
        bool is_current = false;
        for (const auto& tc : m.tool_calls)
            if (tc.value("id", "") == current_id) { is_current = true; break; }
        if (is_current) continue;
        for (const auto& tc : m.tool_calls) {
            auto func = tc.value("function", json::object());
            if (func.value("name", "") != fn) continue;
            const json& raw = func.value("arguments", json::object());
            json stored_args;
            if (raw.is_string()) {
                try { stored_args = json::parse(raw.get_ref<const std::string&>()); }
                catch (...) { continue; }
            } else if (raw.is_object()) {
                stored_args = raw;
            } else {
                continue;
            }
            if (call_key(fn, stored_args) != needle) continue;

            // Skip if the previous call was denied (user didn't approve it).
            // The agent may retry after the user extends permissions.
            std::string prev_id = tc.value("id", "");
            if (was_prev_call_denied(prev_id, history))
                continue;

            // Look up the outcome of the previous call so the model can
            // decide whether retrying makes sense.
            std::string outcome = prev_call_outcome(prev_id, history);

            std::string preview;
            if (args.is_object()) {
                for (auto it = args.begin(); it != args.end(); ++it) {
                    if (it.value().is_string())
                        preview += it.value().get<std::string>() + " ";
                    else
                        preview += it.key() + " ";
                }
            }
            if (preview.size() > 120) preview.resize(120);
            std::string msg = "You already ran \"";
            msg += fn;
            msg += "\" with these exact parameters (";
            msg += preview;
            msg += "...). That previous execution had status=\"";
            msg += outcome;
            msg += "\". Repeating the same tool call will produce the "
                   "same result. If it already succeeded, move on. "
                   "If it failed, adjust your approach. Do not retry "
                   "the exact same call.";
            return msg;
        }
    }
    return {};
}


} // namespace

// Consult the policy store and host hooks to decide whether a tool call is
// approved. PolicyStore rules (always_allow / always_deny) set the dialog
// default and short-circuit on timeout; the dialog always appears as a
// last-chance safety net. Session grants bypass the dialog entirely.
bool approve_tool(const Tool& tool, const json& args, const AgentHooks& hooks,
                  std::set<std::string>& session_approved,
                  PolicyStore* policy) {
    if (!hooks.on_approval) return false;
    std::string name = tool.name();
    std::string summary = tool.summarize(args);

    // Session grant already exists — skip dialog.
    if (policy && policy->is_granted_session(name))
        return true;

    // Compute the dialog default from the policy store (last choice per tool).
    Approval d = hooks.on_approval(name, args, summary);

    // Process the result and update policy state.
    if (d == Approval::AlwaysAllow) {
        if (policy) policy->set_rule(name, PolicyLevel::AlwaysAllow);
        return true;
    }
    if (d == Approval::AlwaysDeny) {
        if (policy) policy->set_rule(name, PolicyLevel::AlwaysDeny);
        return false;
    }
    if (d == Approval::AllowSession) {
        session_approved.insert(name);
        if (policy) {
            policy->grant_session(name);
            policy->record_choice(name, PolicyLevel::AllowSession);
        }
        return true;
    }
    if (d == Approval::AllowOnce) {
        if (policy) policy->record_choice(name, PolicyLevel::AllowOnce);
        return true;
    }
    return false; // Deny
}

bool dispatch_tool_calls(const json& calls, const Config& cfg,
                         ToolRegistry& registry, const AgentHooks& hooks,
                         ConversationLog& log,
                         std::set<std::string>& session_approved,
                         PolicyStore* policy,
                         Context* context) {
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
                dup = find_duplicate_call(c.fn, c.args, context->get_all(), c.id);
            if (!dup.empty()) {
                c.denied_reason = dup;
            } else if (cfg.mode != agent::AgentMode::Yolo &&
                       cfg.policy_approval &&
                       c.tool->requires_approval(c.args) &&
                       !session_approved.count(c.fn) &&
                       !(hooks.on_approval &&
                         approve_tool(*c.tool, c.args, hooks,
                                      session_approved, policy))) {
                c.denied_reason = "denied by user: " + c.fn + " was not approved.";
                log.event("tool_denied", {{"name", c.fn}, {"id", c.id},
                                          {"args", c.args}});
            } else {
                c.approved = true;
            }
        }
        todo.push_back(std::move(c));
    }

    struct Pending {
        size_t idx;
        std::future<ToolResult> future;
    };
    std::vector<Pending> pending;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (!todo[i].approved) continue;
        pending.push_back({i, std::async(std::launch::async, [&todo, i]() {
            try { return todo[i].tool->execute(todo[i].args); }
            catch (const std::exception& e) {
                return ToolResult{false, "", std::string("tool threw: ") + e.what(), agent::json{}};
            }
        })});
    }

    bool all_ok = true;

    auto process_one = [&](const Call& c, ToolResult res) {
        if (!res.ok) all_ok = false;
        if (hooks.on_tool_result) hooks.on_tool_result(c.fn, res);
        if (hooks.on_debug)
            hooks.on_debug("tool_result: " + c.fn + " (" +
                            (res.ok ? "ok" : "error") + ")");
        log.event("tool_result", {{"name", c.fn}, {"id", c.id},
                                    {"ok", res.ok},
                                    {"output", res.ok ? res.output : res.error}});

        json call_args = c.args;
        Message tool_msg;
        tool_msg.role = "tool";
        tool_msg.tool_call_id = c.id;
        tool_msg.name = c.fn;
        tool_msg.content = utf8_sanitize(
            format_tool_envelope(c.fn, call_args, res));
        context->push(std::move(tool_msg));
    };

    // Process non-approved calls immediately (no execution needed).
    for (auto& c : todo) {
        if (c.approved) continue;
        ToolResult res;
        if (c.args_ok) {
            res.ok = false;
            res.error = c.denied_reason;
            res.meta["denied"] = true;
        } else {
            res.ok = false;
            std::string raw = c.args.is_string() ? c.args.get<std::string>()
                                                   : c.args.dump();
            res.error = "tool call arguments were not valid JSON (truncated or "
                        "malformed): " + raw.substr(0, 200);
        }
        process_one(c, std::move(res));
    }

    // Process approved calls as they complete (out-of-order).
    while (!pending.empty()) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            Call& c = todo[it->idx];
            ToolResult res = it->future.get();
            process_one(c, std::move(res));
            pending.erase(it);
            break;
        }
        if (!pending.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return all_ok;
}

} // namespace agent
