#include "tool_process_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_process_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // Three tools behind one binding: one contribution, one ledger entry, so
    // they are installed and removed together.
    caps.push_back(std::make_unique<ToolCapability>(
        "process",
        [](PluginServices& services) -> std::vector<std::unique_ptr<Tool>> {
            if (!services.host || !services.host->jobs)
                return {};
            return make_process_tools(*services.host->jobs);
        },
        ToolCapability::Meta{{{"process_start", {"spawning", ToolRole::Execute}},
                              {"process_read", {"reading", ToolRole::Execute}},
                              {"process_stop", {"stopping", ToolRole::Execute}}}}));
    caps.push_back(make_tool_doc_capability("process_doc", tool_doc_priority::kProcess,
                                            "prompts/tools/process.md"));
    return caps;
}

} // namespace agent::plugins
