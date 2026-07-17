// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_AGENT_H_ALL
#define AGENT_AGENT_H_ALL

// Umbrella header for libagent consumers (TUI, tests, headless CLI).
#include "agent/tool.h"
#include "agent/config.h"
#include "agent/llm.h"
#include "agent/registry.h"
#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/statusbar.h"
#include "agent/session.h"
#include "agent/workspace.h"

namespace agent {
// Register the built-in tools (read, write, search) into the given registry.
// Defined in lib/tools_default.cpp, linked into libagent.
void register_default_tools(ToolRegistry& reg);
}

#endif // AGENT_AGENT_H_ALL
