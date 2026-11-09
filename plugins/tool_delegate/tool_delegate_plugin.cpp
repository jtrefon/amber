#include "tool_delegate_plugin.h"

#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_delegate_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "delegate",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.host || !services.host->subagents)
                return {};
            return wrap_tool(make_delegate_tool(*services.host->subagents, services.tools()));
        },
        ToolCapability::Meta{{{"delegate", {"delegating", ToolRole::Delegate}}}}));
    caps.push_back(make_tool_doc_capability("delegate_doc", tool_doc_priority::kDelegate,
                                            "prompts/tools/delegate.md"));
    return caps;
}

} // namespace agent::plugins
