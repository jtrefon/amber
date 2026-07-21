// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_DISPATCH_H
#define AGENT_DISPATCH_H

#include <string>
#include <vector>

#include "agent/agent.h"  // Agent, Tool, ToolResult, json, AgentHooks
#include "agent/conversation_log.h"
#include "agent/registry.h"
#include "agent/llm.h"

namespace agent {

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
