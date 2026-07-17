// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/registry.h"
#include "agent/tools.h"

namespace agent {

// Register the built-in tools. Kept separate so the TUI and any other host can
// share the exact same tool set without duplicating construction. The tool
// translation units (read_tool.cpp, write_tool.cpp, search_tool.cpp, and the
// search backends) are compiled separately and linked into libagent.
void register_default_tools(ToolRegistry& reg) {
    reg.register_tool(make_read_tool());
    reg.register_tool(make_write_tool());
    reg.register_tool(make_search_tool());
}

} // namespace agent
