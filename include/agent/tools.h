// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <memory>
#include "agent/tool.h"

namespace agent {

// Built-in tool factories. Definitions live in tools/*.cpp, compiled and linked
// into libagent. Kept as factories so the registry owns unique instances.
std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_write_tool();
std::unique_ptr<Tool> make_search_tool();

} // namespace agent

#endif // AGENT_TOOLS_H
