
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

// Consult the decision engine and, when it prompts, the host hook. Records
// the outcome (session grant / persisted rule) against the decision's scope
// id. Returns true only when the call may run; with no hook set, approval
// always fails (fail-safe).
bool approve_tool(const Tool& tool, const json& args, const Config& cfg,
                  const AgentHooks& hooks,
                  std::set<std::string>& session_approved,
                  PolicyStore* policy);

// Execute every requested tool call, recording results into `history`. Tools
// run in parallel via std::async; approval is checked synchronously. Returns
// false if any call failed (error, unknown tool, or denied). The `Call` struct,
// approval gating, and parallel execution are encapsulated here so Agent::run
// stays a thin orchestrator.
bool dispatch_tool_calls(const json& calls, const Config& cfg,
                         ToolRegistry& registry, const AgentHooks& hooks,
                         ConversationLog& log,
                         std::set<std::string>& session_approved,
                         PolicyStore* policy,
                         Context* context);

} // namespace agent

#endif // AGENT_DISPATCH_H
