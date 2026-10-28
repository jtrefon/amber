#include "tool_context_plugin.h"

#include "agent/context_shape.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_context_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "context",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            // A host that never wired a queue would install a tool that quietly
            // does nothing; declining keeps the schema out of the prompt too.
            if (!services.host || !services.host->shape)
                return {};
            return wrap_tool(make_context_tool(*services.host->shape));
        },
        ToolCapability::Meta{{{"context", {"shaping", ToolRole::Control}}}}));
    caps.push_back(make_tool_doc_capability("context_doc", tool_doc_priority::kContext,
                                            "prompts/tools/context.md"));
    return caps;
}

} // namespace agent::plugins
