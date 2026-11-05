#ifndef AGENT_TASK_FORMAT_H
#define AGENT_TASK_FORMAT_H

#include "agent/task.h"

#include <cstdint>
#include <string>

namespace agent {

// Duration for a task, using ended_ms when terminal and the wall clock while
// running. Empty when the task never started.
std::string format_task_duration(std::int64_t started_ms, std::int64_t ended_ms);

// One line per task for an inventory: id | kind | state | elapsed | label |
// progress. Progress is omitted when empty. Bounded by the record's own caps.
std::string format_task_row(const TaskRecord& record);

// The full model/host-facing record, one field per line. Bounded fields are
// already clipped by TaskService; output is served separately by `tasks output`.
std::string format_task_detail(const TaskRecord& record);

} // namespace agent

#endif // AGENT_TASK_FORMAT_H
