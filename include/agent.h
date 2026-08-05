
#ifndef AGENT_AGENT_H_ALL
#define AGENT_AGENT_H_ALL

// Umbrella header for libagent consumers (TUI, tests, headless CLI).
#include "agent/todo.h"
#include "agent/version.h"
#include "agent/tool.h"
#include "agent/config.h"
#include "agent/llm.h"
#include "agent/registry.h"
#include "agent/agent.h"
#include "agent/prompt.h"
#include "agent/statusbar.h"
#include "agent/session.h"
#include "agent/workspace.h"
#include "agent/agent_helpers.h"
#include "agent/job.h"
#include "agent/tools.h"

namespace agent {

// Register the built-in tools into the given registry. `jobs` is the shared
// JobService the model-driven process_* tools operate on; the host owns the
// instance so it stays visible (and killable) from the UI.
// `todos` is the host-owned task list for the todowrite tool; `cancel_token`
// is passed to tools that need cooperative cancellation (bash). Defined in
// lib/tools_default.cpp, linked into libagent.
void register_default_tools(ToolRegistry& reg, JobService& jobs,
                            TodoStore& todos,
                            const CancellationToken& cancel_token = {},
                            bool enable_plan_tool = false);
}

#endif // AGENT_AGENT_H_ALL
