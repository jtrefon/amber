
#ifndef AGENT_DISPATCH_H
#define AGENT_DISPATCH_H

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

class Context;
class PolicyStore;
class Tool;
class ToolRegistry;
struct Config;
struct AgentHooks;
class ConversationLog;
class EventBus;
struct Message;

using json = nlohmann::json;

// The outcome of the approval gate: whether the call may run, and — when it may
// not — why, in one sentence the model can relay. Every refusal path fills the
// reason in: a denial the model cannot explain is a denial it can only
// paraphrase as "it was refused", after which it starts guessing.
struct ApprovalDecision {
    bool allowed = false;
    std::string reason;
};

// Consult the decision engine and, when it prompts, the host hook. Records
// the outcome (session grant / persisted rule) against the decision's scope
// id. With no hook set, approval always fails closed and says so.
ApprovalDecision approve_tool(const Tool& tool, const json& args, const Config& cfg,
                              const AgentHooks& hooks, std::set<std::string>& session_approved,
                              PolicyStore* policy);

// One dispatch: the requested calls plus every collaborator they need. Named
// fields replace the nine positional arguments the function used to carry, and
// the three optional sinks default to "not attached" instead of a row of
// nullptr at each call site.
struct DispatchRequest {
    // cppcheck-suppress uninitMemberVarNoCtor -- reference member; aggregate init binds it.
    const json& calls;
    const Config& cfg;
    ToolRegistry& registry;
    const AgentHooks& hooks;
    ConversationLog& log;
    // cppcheck-suppress uninitMemberVarNoCtor -- reference member; aggregate init binds it.
    std::set<std::string>& session_approved;
    PolicyStore* policy = nullptr;
    EventBus* events = nullptr;
    Context& context;
};

// Execute every requested tool call, recording results into `history`. Tools
// run in parallel via std::async; approval is checked synchronously. Returns
// false if any call failed (error, unknown tool, or denied). The `Call` struct,
// approval gating, and parallel execution are encapsulated here so Agent::run
// stays a thin orchestrator.
bool dispatch_tool_calls(const DispatchRequest& request);

} // namespace agent

#endif // AGENT_DISPATCH_H
