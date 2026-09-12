#include "tool_write_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_write_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "write", [](PluginServices&) { return wrap_tool(make_write_tool()); },
        ToolCapability::Meta{{{"write", {"writing", ToolRole::Write}}}}));
    caps.push_back(
        make_tool_doc_capability("write_doc", tool_doc_priority::kWrite, "prompts/tools/write.md"));
    return caps;
}

} // namespace agent::plugins
