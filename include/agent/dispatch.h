// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_DISPATCH_H
#define AGENT_DISPATCH_H

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

class ToolRegistry;
struct Config;
struct AgentHooks;
class ConversationLog;
struct Message;

using json = nlohmann::json;

// Execute every requested tool call, recording results into `history`. Tools
// run in parallel via std::async; approval is checked synchronously. Returns
// false if any call failed (error, unknown tool, or denied). The `Call` struct,
// approval gating, and parallel execution are encapsulated here so Agent::run
// stays a thin orchestrator.
bool dispatch_tool_calls(const json& calls, const Config& cfg,
                         ToolRegistry& registry, const AgentHooks& hooks,
                         ConversationLog& log,
                         std::set<std::string>& session_approved,
                         std::vector<Message>& history);

} // namespace agent

#endif // AGENT_DISPATCH_H
