#ifndef AGENT_TASK_JOB_H
#define AGENT_TASK_JOB_H

#include "agent/job.h"
#include "agent/task.h"

#include <string>

namespace agent {

// The adapter from the command backend to the one task state machine. A job
// that exits is Succeeded or Failed by its exit code; a killed job is TimedOut
// when its own deadline fired and Interrupted otherwise. A job that never
// reached a running state is Failed.
TaskState task_state_from_job(const JobInfo& info);
TaskResult task_result_from_job(const JobInfo& info);
std::string task_error_from_job(const JobInfo& info);

} // namespace agent

#endif // AGENT_TASK_JOB_H
