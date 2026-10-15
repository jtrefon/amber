#ifndef AGENT_PLUGINS_PROJECT_INDEX_TOOLS_H
#define AGENT_PLUGINS_PROJECT_INDEX_TOOLS_H

#include "agent/tool.h"

#include <memory>
#include <vector>

namespace agent::plugins {

// The tools that write and read the project index. Five small tools rather than
// one verb-switch: the model picks the verb from the name, the approval gate can
// be per-call (index_init proposes for free and asks only when it writes), and
// the toolset audit sees the shape of the set.
std::vector<std::unique_ptr<Tool>> make_index_tools();

} // namespace agent::plugins

#endif // AGENT_PLUGINS_PROJECT_INDEX_TOOLS_H
