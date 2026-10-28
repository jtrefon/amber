
#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <memory>
#include <string>
#include <vector>
#include "agent/tool.h"
#include "agent/process.h"
#include "agent/registry.h"
#include "agent/search_backend.h"

namespace agent {

class JobService; // process_* tools bind to the host-owned job service
class SkillCatalog;
class ShapeQueue;       // context tool binds to the host-owned shaping queue
class TodoStore;        // todowrite tool binds to the host-owned task list
class SubAgentExecutor; // task tool binds to the host-owned sub-agent executor

// Built-in tool factories. Definitions live in tools/*.cpp, compiled and linked
// into libagent. Kept as factories so the registry owns unique instances.
std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_read_result_tool();
std::unique_ptr<Tool> make_write_tool();
// The search tool resolves `mode` through the provider handed in at
// construction: a hosted session passes the runtime's live registry, a bare
// host the built-in pair (search_backend.h), so the tool never holds a raw
// registry pointer and names no backend itself.
std::unique_ptr<Tool> make_search_tool(SearchBackendProvider provider);
std::unique_ptr<Tool> make_todowrite_tool(TodoStore& todos);
// The model's own trim: it names evidence the conversation has stopped needing
// and the harness applies it at the next request boundary.
std::unique_ptr<Tool> make_context_tool(ShapeQueue& shapes);
std::unique_ptr<Tool> make_task_tool(SubAgentExecutor& executor, ToolRegistry& registry);
std::unique_ptr<Tool> make_bash_tool(JobService* jobs = nullptr,
                                     const CancellationToken& cancel_token = {});

// Process (background job) tool factories. They all share the caller-owned
// JobService so model-started jobs are visible and killable from the host.
std::vector<std::unique_ptr<Tool>> make_process_tools(JobService& jobs);

// A capability factory returns a list; most tools are one. Empty in, empty out,
// so a factory that declines still reads as "no tools" rather than a null
// entry.
std::vector<std::unique_ptr<Tool>> wrap_tool(std::unique_ptr<Tool> tool);

// Skill tools resolve the RUNNING agent's catalog through the run scope
// (run_scope.h), so they hold no catalog and can be installed by the
// tool_skills plugin like any other tool. Outside a session they report that
// instead of serving a catalog nobody asked for.
std::unique_ptr<Tool> make_read_skill_tool();
std::unique_ptr<Tool> make_list_skills_tool();
std::unique_ptr<Tool> make_write_skill_tool();

// Author a SKILL.md into the given scope (project|global). Shared by
// write_skill and /set skills create. Returns an empty string on success or a
// human-readable error message.
std::string author_skill(SkillCatalog& catalog, const std::string& name,
                         const std::string& description, const std::string& body,
                         const std::string& scope);

} // namespace agent

#endif // AGENT_TOOLS_H
