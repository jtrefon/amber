
#include "agent/dispatch.h"
#include "agent/events.h"
#include "agent/agent_helpers.h"
#include "agent/context.h"
#include "agent/policy.h"
#include "agent/policy_engine.h"
#include "agent/run_scope.h"
#include "agent/subagent.h"

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
std::string prev_call_outcome(const std::string& tool_call_id, const std::deque<Message>& history) {
    if (tool_call_id.empty())
        return "unknown";
    for (const auto& m : history) {
        if (m.role != "tool")
            continue;
        if (m.tool_call_id != tool_call_id)
            continue;
        // Envelope: [tool=name args=... status=X meta=...]\n...
        auto pos = m.content.find("status=");
        if (pos == std::string::npos)
            return "unknown";
        pos += 7; // skip "status="
        auto end = m.content.find(' ', pos);
        if (end == std::string::npos)
            end = m.content.find(']', pos);
        if (end == std::string::npos)
            return "unknown";
        return m.content.substr(pos, end - pos);
    }
    return "unknown";
}

// Check if a previous tool call was denied (not executed) by looking for
// the corresponding tool result message in history. Returns true if the
// previous call matching the given `tool_call_id` was denied by policy.
bool was_prev_call_denied(const std::string& tool_call_id, const std::deque<Message>& history) {
    if (tool_call_id.empty())
        return false;
    return prev_call_outcome(tool_call_id, history) == "denied";
}

// True when this assistant message contains the tool call being dispatched now.
bool mentions_call_id(const Message& m, const std::string& current_id) {
    return std::any_of(m.tool_calls.begin(), m.tool_calls.end(),
                       [&](const json& tc) { return tc.value("id", "") == current_id; });
}

// The arguments recorded for a previous tool call. They are stored either as a
// JSON string (the wire form) or already as an object; anything else is
// unusable and the call is skipped.
bool parse_stored_args(const json& raw, json& stored_args) {
    if (raw.is_string()) {
        try {
            stored_args = json::parse(raw.get_ref<const std::string&>());
            return true;
        } catch (...) {
            return false;
        }
    }
    if (raw.is_object()) {
        stored_args = raw;
        return true;
    }
    return false;
}

// A short readable summary of the repeated arguments, capped so the message
// stays small.
std::string duplicate_preview(const json& args) {
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
    return preview;
}

// The message the model gets when it repeats a call verbatim.
std::string duplicate_message(const std::string& fn, const std::string& preview,
                              const std::string& outcome) {
    return "You already ran \"" + fn + "\" with these exact parameters (" + preview +
           "...). That previous execution had status=\"" + outcome +
           "\". Repeating the same tool call will produce the "
           "same result. If it already succeeded, move on. "
           "If it failed, adjust your approach. Do not retry "
           "the exact same call.";
}

// Check if a tool call (name + arguments) already exists in a PRIOR
// assistant message — one whose tool_calls do NOT include the current
// call's `id`.  This prevents a batch of calls in the current turn from
// matching each other while still catching genuine repeats across turns.
// Calls that were previously DENIED (not executed) are skipped, allowing
// retries after the user extends permissions.
// Returns a descriptive message string if duplicate, empty string if not.
std::string find_duplicate_call(const std::string& fn, const json& args,
                                const std::deque<Message>& history, const std::string& current_id) {
    const std::string needle = call_key(fn, args);
    for (const auto& m : history) {
        if (m.role != "assistant" || m.tool_calls.is_null())
            continue;
        if (mentions_call_id(m, current_id))
            continue; // the call being dispatched right now
        for (const auto& tc : m.tool_calls) {
            const auto func = tc.value("function", json::object());
            if (func.value("name", "") != fn)
                continue;
            json stored_args;
            if (!parse_stored_args(func.value("arguments", json::object()), stored_args))
                continue;
            if (call_key(fn, stored_args) != needle)
                continue;

            // Skip if the previous call was denied (user didn't approve it).
            // The agent may retry after the user extends permissions.
            const std::string prev_id = tc.value("id", "");
            if (was_prev_call_denied(prev_id, history))
                continue;

            // The previous call's outcome tells the model whether retrying
            // makes sense.
            return duplicate_message(fn, duplicate_preview(args),
                                     prev_call_outcome(prev_id, history));
        }
    }
    return {};
}

} // namespace

// Gate one tool call: apply the decision engine, and when the verdict is
// Prompt, consult the host hook and record the outcome against the scope.
// Returns true when the call may run. With no host hook the call is denied
// (fail-safe).
namespace {

bool is_grant(Approval d) {
    return d == Approval::AllowOnce || d == Approval::AllowSession || d == Approval::AlwaysAllow;
}

// Record the user's answer against the policy, keyed by the scope so an
// "always allow" for `rm` never silently approves `dd` (and never asks again for
// the same command kind). Returns whether the call may run.
bool record_decision(Approval d, const std::string& scope, PolicyStore* policy,
                     std::set<std::string>& session_approved) {
    if (d == Approval::AlwaysAllow) {
        policy->set_rule(scope, PolicyLevel::AlwaysAllow);
        return true;
    }
    if (d == Approval::AlwaysDeny) {
        policy->set_rule(scope, PolicyLevel::AlwaysDeny);
        return false;
    }
    if (d == Approval::AllowSession) {
        session_approved.insert(scope);
        policy->grant_session(scope);
        policy->record_choice(scope, PolicyLevel::AllowSession);
        return true;
    }
    if (d == Approval::AllowOnce) {
        policy->record_choice(scope, PolicyLevel::AllowOnce);
        return true;
    }
    return false; // Deny
}

} // namespace

bool approve_tool(const Tool& tool, const json& args, const Config& cfg, const AgentHooks& hooks,
                  std::set<std::string>& session_approved, PolicyStore* policy) {
    if (!hooks.on_approval)
        return false; // fail-safe: no host, no approval
    const std::string summary = tool.summarize(args);
    // No policy store: fall back to the legacy whole-tool dialog, with no rule
    // to remember the answer against.
    if (!policy) {
        Approval d = hooks.on_approval(tool.name(), args, summary);
        if (d == Approval::AllowSession)
            session_approved.insert(tool.name());
        return is_grant(d);
    }
    Decision dec = decide_approval(cfg, tool, args, *policy);
    if (dec.v != Verdict::Prompt)
        return dec.v == Verdict::Allow;

    return record_decision(hooks.on_approval(tool.name(), args, summary), dec.scope_id, policy,
                           session_approved);
}

namespace {

// One requested tool call, from parse through approval.
struct Call {
    std::string id, fn;
    json args;
    bool args_ok = true;
    std::shared_ptr<Tool> tool; // lease: alive across concurrent unregister
    bool approved = false;
    bool intercepted = false; // an event interceptor blocked this call
    std::string denied_reason;
};

struct Pending {
    size_t idx = 0;
    std::future<ToolResult> future;
};

// Tool events are optional: no bus attached means every publish is skipped, and
// dispatch pays nothing for the plugin surface.
template <class E> void publish_event(EventBus* events, E& event) {
    if (events)
        Events(*events).publish(event);
}

// "tool_denied" carrying a machine reason (interceptor / read_mode).
void log_denied(ConversationLog& log, const Call& c, const char* reason) {
    log.event("tool_denied", {{"name", c.fn}, {"id", c.id}, {"reason", reason}});
}

// "tool_denied" for a rejected approval: the arguments identify the call.
void log_denied_with_args(ConversationLog& log, const Call& c) {
    log.event("tool_denied", {{"name", c.fn}, {"id", c.id}, {"args", c.args}});
}

// Interceptors see every requested call before the approval gate: they can
// rewrite the arguments or veto the call outright. This fires before approval,
// so it means "the model asked", not "this will run".
void run_interceptors(EventBus* events, ConversationLog& log, Call& c) {
    if (!events)
        return;
    ToolRequestedEvent requested;
    requested.name = c.fn;
    requested.args = c.args;
    publish_event(events, requested);
    if (requested.cancel) {
        c.intercepted = true;
        c.denied_reason = "denied by interceptor: " + c.fn;
        log_denied(log, c, "interceptor");
    } else if (c.args_ok) {
        c.args = requested.args;
    }
}

// Duplicate detection: skip when disabled (/set detection duplicate off).
std::string duplicate_reason(const Config& cfg, const Call& c, Context* context) {
    if (!cfg.detection_duplicate)
        return "";
    return find_duplicate_call(c.fn, c.args, context->get_all(), c.id);
}

// The decision engine: mode (Yolo bypass / Read deny), tool contract, shell
// classifier (benign writes free), stored always-allow/deny, and per-scope
// session grants. Only a Prompt verdict reaches the host dialog.
bool approve_with_policy(const Config& cfg, const Call& c, const AgentHooks& hooks,
                         std::set<std::string>& session_approved, PolicyStore* policy,
                         ConversationLog& log) {
    Decision dec = decide_approval(cfg, *c.tool, c.args, *policy);
    bool approved = dec.v == Verdict::Allow;
    if (dec.v == Verdict::Prompt)
        approved = approve_tool(*c.tool, c.args, cfg, hooks, session_approved, policy);
    if (!approved)
        log_denied_with_args(log, c);
    return approved;
}

// No policy store: Yolo bypasses, otherwise the tool's own contract decides,
// falling back to the host dialog.
bool approve_without_policy(const Config& cfg, const Call& c, const AgentHooks& hooks,
                            std::set<std::string>& session_approved, ConversationLog& log) {
    if (cfg.mode == agent::AgentMode::Yolo || !c.tool->requires_approval(c.args) ||
        (hooks.on_approval &&
         approve_tool(*c.tool, c.args, cfg, hooks, session_approved, /*policy=*/nullptr)))
        return true;
    log_denied_with_args(log, c);
    return false;
}

// Parse one requested call, announce it, let interceptors see it, resolve the
// tool, then run the approval gate. The result carries either `approved` or a
// `denied_reason`.
Call prepare_call(const json& raw, const Config& cfg, ToolRegistry& registry,
                  const AgentHooks& hooks, ConversationLog& log,
                  std::set<std::string>& session_approved, PolicyStore* policy, EventBus* events,
                  Context* context) {
    Call c;
    parse_tool_call(raw, c.id, c.fn, c.args, c.args_ok);
    if (hooks.on_tool_call)
        hooks.on_tool_call(c.fn, c.args);
    if (hooks.on_debug)
        hooks.on_debug("tool_call: " + c.fn);
    log.event("tool_call", {{"name", c.fn}, {"id", c.id}, {"args", c.args}});

    run_interceptors(events, log, c);

    c.tool = registry.find(c.fn); // shared lease: survives unregister mid-dispatch
    if (c.intercepted) {
        // Denial already recorded; the call still produces a result below.
    } else if (!c.tool) {
        c.denied_reason = "unknown tool: " + c.fn;
    } else if (cfg.mode == agent::AgentMode::Read && !c.tool->is_read_only()) {
        c.denied_reason = "tool \"" + c.fn + "\" is not available in read mode";
        log_denied(log, c, "read_mode");
    } else if (c.args_ok) {
        std::string dup = duplicate_reason(cfg, c, context);
        if (!dup.empty()) {
            c.denied_reason = dup;
        } else if (policy) {
            c.approved = approve_with_policy(cfg, c, hooks, session_approved, policy, log);
        } else {
            c.approved = approve_without_policy(cfg, c, hooks, session_approved, log);
        }
    }
    return c;
}

// Launch every approved call on a worker so they run concurrently. Nesting
// state must survive the thread hop: the task tool runs inside sub-agents on
// dispatch workers, and a nested task call would otherwise bypass the
// in_subagent guard. Each worker inherits the caller's state. The run scope
// lives on the calling (agent) thread; tools execute on workers that would
// otherwise see none, so it is installed on each worker to keep cancellation,
// catalog resolution and activation recording following the CALLING agent.
std::vector<Pending> launch_approved(std::vector<Call>& todo, bool caller_in_subagent,
                                     const RunScopeChain& active_scope) {
    std::vector<Pending> pending;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (!todo[i].approved)
            continue;
        pending.push_back(
            {i, std::async(std::launch::async, [&todo, i, caller_in_subagent, active_scope]() {
                 ScopedRunScope scope_guard(active_scope);
                 set_subagent_inherited(caller_in_subagent);
                 try {
                     return todo[i].tool->execute(todo[i].args);
                 } catch (const std::exception& e) {
                     return ToolResult{false, "", std::string("tool threw: ") + e.what(),
                                       agent::json{}};
                 }
             })});
    }
    return pending;
}

// The result recorded for a call that never ran: the denial reason, or the
// argument-parse failure when the model emitted malformed JSON.
ToolResult denied_result(const Call& c) {
    ToolResult res;
    res.ok = false;
    if (c.args_ok) {
        res.error = c.denied_reason;
        res.meta["denied"] = true;
        return res;
    }
    std::string raw = c.args.is_string() ? c.args.get<std::string>() : c.args.dump();
    res.error =
        "tool call arguments were not valid JSON (truncated or malformed): " + raw.substr(0, 200);
    return res;
}

// Record one finished call: publish the result, notify the host, and append the
// tool message to the context. Returns the tool's own success flag.
bool finish_call(const Call& c, ToolResult res, const AgentHooks& hooks, ConversationLog& log,
                 EventBus* events, Context* context) {
    ToolCompletedEvent completed;
    completed.name = c.fn;
    completed.result = &res;
    publish_event(events, completed);
    if (hooks.on_tool_result)
        hooks.on_tool_result(c.fn, res, c.args);
    if (hooks.on_debug)
        hooks.on_debug("tool_result: " + c.fn + " (" + (res.ok ? "ok" : "error") + ")");
    log.event("tool_result", {{"name", c.fn},
                              {"id", c.id},
                              {"ok", res.ok},
                              {"output", res.ok ? res.output : res.error}});

    Message tool_msg;
    tool_msg.role = "tool";
    tool_msg.tool_call_id = c.id;
    tool_msg.name = c.fn;
    tool_msg.content = utf8_sanitize(format_tool_envelope(c.fn, c.args, res));
    context->push(std::move(tool_msg));
    MessageAddedEvent added;
    added.message = &context->get_all().back();
    added.index = context->get_all().size() - 1;
    publish_event(events, added);
    return res.ok;
}

} // namespace

// Prepare every call (parse, interceptors, approval), run the approved ones
// concurrently, and record the results. Non-approved calls produce a result
// immediately — there is nothing to execute.
bool dispatch_tool_calls(const json& calls, const Config& cfg, ToolRegistry& registry,
                         const AgentHooks& hooks, ConversationLog& log,
                         std::set<std::string>& session_approved, PolicyStore* policy,
                         EventBus* events, Context* context) {
    std::vector<Call> todo;
    todo.reserve(calls.size());
    for (const auto& call : calls)
        todo.push_back(prepare_call(call, cfg, registry, hooks, log, session_approved, policy,
                                    events, context));

    const bool caller_in_subagent = in_subagent();
    const RunScopeChain active_scope = capture_run_scope();
    std::vector<Pending> pending = launch_approved(todo, caller_in_subagent, active_scope);

    bool all_ok = true;

    // Non-approved calls: report the denial (or the malformed-arguments failure).
    for (const auto& c : todo) {
        if (c.approved)
            continue;
        if (!finish_call(c, denied_result(c), hooks, log, events, context))
            all_ok = false;
    }

    // Approved calls are processed as they complete, out of order.
    while (!pending.empty()) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            Call& c = todo[it->idx];
            ToolResult res = it->future.get();
            if (!finish_call(c, std::move(res), hooks, log, events, context))
                all_ok = false;
            pending.erase(it);
            break;
        }
        if (!pending.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return all_ok;
}

} // namespace agent
