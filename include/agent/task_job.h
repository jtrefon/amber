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

// The spawn request shared by the model's process_start tool and the host's
// /tasks start command. The owner is captured by the caller because the UI has
// a window identity rather than a run scope.
struct CommandTaskStart {
    std::string command;
    std::string cwd;
    std::string label;
    TaskOwner owner;
    TaskNotify notify = TaskNotify::Auto;
    long hard_timeout_s = 600;
    long idle_timeout_s = 30;
};

struct StartedCommandTask {
    std::string task_id;
    std::string backend_id;
};

// Create the task, start the job behind it, bind the two and attach the
// process-group cancel handler. Empty task_id with error set on refusal or
// spawn failure. The TaskService must outlive the JobService.
StartedCommandTask start_command_task(JobService& jobs, TaskService& tasks,
                                      const CommandTaskStart& options,
                                      std::string* error = nullptr);

} // namespace agent

#endif // AGENT_TASK_JOB_H
