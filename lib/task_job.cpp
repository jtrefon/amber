#include "agent/task_job.h"

namespace agent {

TaskState task_state_from_job(const JobInfo& info) {
    if (info.state == JobState::Done)
        return info.exit_code == 0 ? TaskState::Succeeded : TaskState::Failed;
    if (info.state == JobState::Killed) {
        if (info.remaining_idle_s == 0 || info.remaining_hard_s == 0)
            return TaskState::TimedOut;
        return TaskState::Interrupted;
    }
    return TaskState::Failed;
}

TaskResult task_result_from_job(const JobInfo& info) {
    TaskResult result;
    result.exit_code = info.exit_code;
    const TaskState state = task_state_from_job(info);
    if (state == TaskState::Succeeded)
        result.summary = "exit 0";
    else if (state == TaskState::Failed)
        result.summary = "exit " + std::to_string(info.exit_code);
    else if (state == TaskState::TimedOut)
        result.summary = "timed out";
    else if (state == TaskState::Interrupted)
        result.summary = "interrupted";
    return result;
}

std::string task_error_from_job(const JobInfo& info) {
    const TaskState state = task_state_from_job(info);
    if (state == TaskState::TimedOut)
        return "timed out";
    if (state == TaskState::Interrupted)
        return "interrupted";
    if (state == TaskState::Failed)
        return "exit code " + std::to_string(info.exit_code);
    return {};
}

} // namespace agent
