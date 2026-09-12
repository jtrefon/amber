#include "tool_search_plugin.h"

#include "agent/job.h"
#include "agent/todo.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_search_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "search", [](PluginServices&) { return wrap_tool(make_search_tool()); },
        ToolCapability::Meta{{{"search", {"searching", ToolRole::Search}}}}));
    caps.push_back(make_tool_doc_capability("search_doc", tool_doc_priority::kSearch,
                                            "prompts/tools/search.md"));
    return caps;
}

} // namespace agent::plugins
