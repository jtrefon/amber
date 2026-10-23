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
        ToolCapability::Meta{{{"read", {"reading", ToolRole::Read}}}}));
    caps.push_back(std::make_unique<ToolCapability>(
        "read_result", [](PluginServices&) { return wrap_tool(make_read_result_tool()); },
        ToolCapability::Meta{{{"read_result", {"reading", ToolRole::Read}}}}));
    caps.push_back(
        make_tool_doc_capability("read_doc", tool_doc_priority::kRead, "prompts/tools/read.md"));
    caps.push_back(make_tool_doc_capability("read_result_doc", tool_doc_priority::kReadResult,
                                            "prompts/tools/read_result.md"));
    return caps;
}

} // namespace agent::plugins
