#include "agent/tools.h"

#include "agent/job.h"
#include "agent/json_read.h"
#include "agent/task.h"
#include "agent/task_job.h"
#include "agent/workspace.h"

#include <chrono>
#include <string>

namespace agent {

namespace {

TaskNotify parse_notify(const std::string& value) {
    if (value == "digest")
        return TaskNotify::Digest;
    if (value == "silent")
        return TaskNotify::Silent;
    return TaskNotify::Auto;
}

} // namespace

class ProcessStartTool final : public Tool {
public:
    explicit ProcessStartTool(JobService& jobs, TaskService* tasks = nullptr)
        : jobs_(jobs), tasks_(tasks) {}

    std::string name() const noexcept override { return "process_start"; }
    std::string description() const noexcept override;
    json parameters_schema() const override;
    bool requires_approval(const json&) const noexcept override { return true; }
    bool is_read_only() const noexcept override { return false; }
    std::string summarize(const json& args) const override;
    ToolResult execute(const json& args) const override;

private:
    ToolResult start_with_task(const std::string& command, long hard, long idle,
                               const std::string& cwd, const std::string& label,
                               TaskNotify notify) const;
    ToolResult start_without_task(const std::string& command, long hard, long idle,
                                  const std::string& cwd) const;

    JobService& jobs_;
    TaskService* tasks_ = nullptr;
};

std::string ProcessStartTool::description() const noexcept {
    return "Start a shell command in the BACKGROUND and return a task id immediately "
           "(non-blocking). Use this for long-running commands such as servers, watchers, "
           "or builds. The process keeps running across turns. Inspect or stop it with "
           "tasks status/output/cancel/wait. Arguments: command (required), timeout (hard "
           "seconds, default 600), idle_timeout (seconds of no output before auto-kill, "
           "default 30), cwd (optional, defaults to workspace root), label (optional), "
           "notify (auto|digest|silent).";
}

json ProcessStartTool::parameters_schema() const {
    json p = json::object();
    p["type"] = "object";
    json props = json::object();
    props["command"] = json::object(
        {{"type", "string"}, {"description", "Shell command to run in the background."}});
    props["timeout"] = json::object(
        {{"type", "integer"}, {"description", "Hard lifetime in seconds (default 600)."}});
    props["idle_timeout"] =
        json::object({{"type", "integer"},
                      {"description", "Seconds of no output before auto-kill (default 30)."}});
    props["cwd"] = json::object(
        {{"type", "string"}, {"description", "Working directory (default: workspace root)."}});
    props["label"] =
        json::object({{"type", "string"}, {"description", "Short human label for task lists."}});
    props["notify"] =
        json::object({{"type", "string"},
                      {"enum", json::array({"auto", "digest", "silent"})},
                      {"description", "When a completion may wake the owner (default auto)."}});
    p["properties"] = props;
    p["required"] = json::array({"command"});
    return p;
}

std::string ProcessStartTool::summarize(const json& args) const {
    std::string command = args.contains("command") && args["command"].is_string()
                              ? args["command"].get<std::string>()
                              : "";
    if (command.size() > 200) {
        command.resize(197);
        command += "...";
    }
    return "bg start: " + command;
}

ToolResult ProcessStartTool::start_without_task(const std::string& command, long hard, long idle,
                                                const std::string& cwd) const {
    const std::string job_id = jobs_.start(command, cwd, hard, idle);
    if (job_id.empty())
        return {false, "", "failed to start background process"};
    return {true, job_id, "", {{"job_id", job_id}}};
}

ToolResult ProcessStartTool::start_with_task(const std::string& command, long hard, long idle,
                                             const std::string& cwd, const std::string& label,
                                             TaskNotify notify) const {
    CommandTaskStart options;
    options.command = command;
    options.cwd = cwd.empty() ? Workspace::workspace_root() : cwd;
    options.label = label;
    options.owner = task_owner_from_scope();
    options.notify = notify;
    options.hard_timeout_s = hard;
    options.idle_timeout_s = idle;
    std::string error;
    const StartedCommandTask started = start_command_task(jobs_, *tasks_, options, &error);
    if (started.task_id.empty())
        return {false, "", error.empty() ? "failed to start background process" : error};
    return {
        true, started.task_id, "", {{"task_id", started.task_id}, {"job_id", started.backend_id}}};
}

ToolResult ProcessStartTool::execute(const json& args) const {
    if (!args.contains("command") || !args["command"].is_string() ||
        args["command"].get<std::string>().empty()) {
        return {false, "", "missing 'command'"};
    }
    const std::string command = args["command"].get<std::string>();
    const long hard = static_cast<long>(read_int(args, "timeout", 600));
    const long idle = static_cast<long>(read_int(args, "idle_timeout", 30));
    const std::string cwd = read_string(args, "cwd", "");
    const std::string label = read_string(args, "label", "");
    const TaskNotify notify = parse_notify(read_string(args, "notify", "auto"));
    if (!tasks_)
        return start_without_task(command, hard, idle, cwd);
    return start_with_task(command, hard, idle, cwd, label, notify);
}

std::unique_ptr<Tool> make_process_start_tool(JobService& jobs, TaskService* tasks) {
    return std::make_unique<ProcessStartTool>(jobs, tasks);
}

} // namespace agent
