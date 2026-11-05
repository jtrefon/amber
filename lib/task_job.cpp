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

namespace {

std::string command_label(const std::string& command) {
    if (command.size() > 80)
        return command.substr(0, 77) + "...";
    return command;
}

} // namespace

StartedCommandTask start_command_task(JobService& jobs, TaskService& tasks,
                                      const CommandTaskStart& options, std::string* error) {
    const std::string label =
        options.label.empty() ? command_label(options.command) : options.label;
    TaskRecord record;
    record.kind = TaskKind::Command;
    record.owner = options.owner;
    record.label = label;
    record.command = options.command;
    record.cwd = options.cwd;
    record.notify = options.notify;

    std::string create_error;
    std::string task_id = tasks.create(std::move(record), &create_error);
    if (task_id.empty()) {
        if (error)
            *error = create_error.empty() ? "task limit reached; do not retry" : create_error;
        return {};
    }
    tasks.started(task_id, 0);

    JobOrigin origin;
    origin.owner_id = options.owner.owner_id;
    origin.label = label;
    const std::string job_id =
        jobs.start(options.command, options.cwd, options.hard_timeout_s, options.idle_timeout_s,
                   std::move(origin), [tasks = &tasks, task_id](const JobInfo& info) {
                       tasks->finish(task_id, task_state_from_job(info), task_result_from_job(info),
                                     task_error_from_job(info));
                   });
    if (job_id.empty()) {
        tasks.finish(task_id, TaskState::Failed, {}, "failed to start background process");
        if (error)
            *error = "failed to start background process";
        return {};
    }
    tasks.bind_backend(task_id, job_id);
    tasks.attach_cancel(
        task_id, [jobs = &jobs, job_id](const CancellationToken&) { return jobs->stop(job_id); });
    return {task_id, job_id};
}

} // namespace agent
