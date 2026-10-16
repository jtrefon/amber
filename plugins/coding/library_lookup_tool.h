#ifndef AGENT_PLUGINS_CODING_LIBRARY_LOOKUP_TOOL_H
#define AGENT_PLUGINS_CODING_LIBRARY_LOOKUP_TOOL_H

#include "agent/tool.h"

#include <memory>
#include <vector>

namespace agent::plugins {

// The library's retrieval tool: an entry's full text by name, or the entries
// matching a query.
//
// It exists because of where the library lives -- with the *install*, not with
// the project's workspace -- so the workspace-confined read and search tools
// cannot reach it. The working index names entries the project's tags select;
// this is how the agent follows a name to the body.
std::vector<std::unique_ptr<Tool>> wrap_library_lookup();

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CODING_LIBRARY_LOOKUP_TOOL_H
