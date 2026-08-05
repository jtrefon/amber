
#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <memory>
#include <string>
#include <vector>
#include "agent/tool.h"
#include "agent/process.h"
#include "agent/registry.h"

namespace agent {

class JobService;  // process_* tools bind to the host-owned job service
class SkillCatalog;
class TodoStore;   // todowrite tool binds to the host-owned task list

// Built-in tool factories. Definitions live in tools/*.cpp, compiled and linked
// into libagent. Kept as factories so the registry owns unique instances.
std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_write_tool();
std::unique_ptr<Tool> make_search_tool();
std::unique_ptr<Tool> make_todowrite_tool(TodoStore& todos);
std::unique_ptr<Tool> make_bash_tool(JobService* jobs = nullptr,
                                     const CancellationToken& cancel_token = {});

// Process (background job) tool factories. They all share the caller-owned
// JobService so model-started jobs are visible and killable from the host.
std::vector<std::unique_ptr<Tool>> make_process_tools(JobService& jobs);

// Skill tools bind to the session SkillCatalog, which they need for lookup,
// body caching/activation, and re-scan on authoring.
std::unique_ptr<Tool> make_read_skill_tool(SkillCatalog& catalog);
std::unique_ptr<Tool> make_list_skills_tool(SkillCatalog& catalog);
std::unique_ptr<Tool> make_write_skill_tool(SkillCatalog& catalog);

// Register the skill tools. Called by the Agent once its catalog exists; the
// catalog outlives the registry (both are session-scoped).
void register_skill_tools(ToolRegistry& reg, SkillCatalog& catalog);

// Author a SKILL.md into the given scope (project|global). Shared by
// write_skill and /set skills create. Returns an empty string on success or a
// human-readable error message.
std::string author_skill(SkillCatalog& catalog, const std::string& name,
                         const std::string& description,
                         const std::string& body, const std::string& scope);

} // namespace agent

#endif // AGENT_TOOLS_H
