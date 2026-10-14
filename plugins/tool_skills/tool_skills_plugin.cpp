#include "tool_skills_plugin.h"

#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_skill_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "read_skill", [](PluginServices&) { return wrap_tool(make_read_skill_tool()); },
        ToolCapability::Meta{{{"read_skill", {"activating", ToolRole::Read}}}}));
    caps.push_back(std::make_unique<ToolCapability>(
        "list_skills", [](PluginServices&) { return wrap_tool(make_list_skills_tool()); },
        ToolCapability::Meta{{{"list_skills", {"listing", ToolRole::Read}}}}));
    caps.push_back(std::make_unique<ToolCapability>(
        "write_skill", [](PluginServices&) { return wrap_tool(make_write_skill_tool()); },
        ToolCapability::Meta{{{"write_skill", {"authoring", ToolRole::Write}}}}));
    caps.push_back(make_tool_doc_capability("skills_doc", tool_doc_priority::kSkills,
                                            "prompts/tools/skills.md"));
    return caps;
}

} // namespace agent::plugins
