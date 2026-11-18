#include "tool_checkpoint_plugin.h"

#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_checkpoint_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "git_checkpoint", [](PluginServices&) { return wrap_tool(make_checkpoint_tool()); },
        ToolCapability::Meta{{{"git_checkpoint", {"checkpointing", ToolRole::Write}}}}));
    caps.push_back(make_tool_doc_capability("checkpoint_doc", tool_doc_priority::kCheckpoint,
                                            "prompts/tools/checkpoint.md"));
    return caps;
}

} // namespace agent::plugins
