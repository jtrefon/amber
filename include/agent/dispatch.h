
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

// Execute every requested tool call, recording results into `history`. Tools
// run in parallel via std::async; approval is checked synchronously. Returns
// false if any call failed (error, unknown tool, or denied). The `Call` struct,
// approval gating, and parallel execution are encapsulated here so Agent::run
// stays a thin orchestrator.
class EventBus;

bool dispatch_tool_calls(const json& calls, const Config& cfg, ToolRegistry& registry,
                         const AgentHooks& hooks, ConversationLog& log,
                         std::set<std::string>& session_approved, PolicyStore* policy,
                         EventBus* events, Context* context);

} // namespace agent

#endif // AGENT_DISPATCH_H
