#include "tool_read_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_read_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "read", [](PluginServices&) { return wrap_tool(make_read_tool()); },
        ToolCapability::Verbs{{{"read", "reading"}}}));
    caps.push_back(
        make_tool_doc_capability("read_doc", tool_doc_priority::kRead, "prompts/tools/read.md"));
    return caps;
}

} // namespace agent::plugins
