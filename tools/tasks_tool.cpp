#include "agent/tools.h"

#include "agent/job.h"
#include "agent/json_read.h"
#include "agent/run_scope.h"
#include "agent/task.h"
#include "agent/task_format.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace agent {

namespace {

constexpr std::size_t kOutputCap = std::size_t{8} * 1024;

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

std::string require_id(const json& args, const char* action, ToolResult& result) {
    const std::string id = args.value("id", std::string());
    if (id.empty()) {
        result.ok = false;
        result.error = std::string("tasks ") + action + ": missing id";
    }
    return id;
}

int bounded_timeout(long long seconds) {
    if (seconds < 1)
        return 1;
    if (seconds > 3600)
        return 3600;
    return static_cast<int>(seconds);
}

std::string state_meta(const TaskRecord& record) {
    return task_state_name(record.state);
}

} // namespace

class TasksTool final : public Tool {
public:
    TasksTool(TaskService& tasks, JobService& jobs, const CancellationToken& fallback_token)
        : tasks_(tasks), jobs_(jobs), fallback_token_(fallback_token) {}

    std::string name() const noexcept override { return "tasks"; }
    std::string description() const noexcept override;
    json parameters_schema() const override;
    bool requires_approval(const json& args) const noexcept override;
    std::string summarize(const json& args) const override;
    ToolResult execute(const json& args) const override;

private:
    std::optional<TaskRecord> owned(const std::string& id) const;
    const CancellationToken& token() const;
    ToolResult list() const;
    ToolResult status(const std::string& id) const;
    std::string output_body(const TaskRecord& record) const;
    ToolResult output(const std::string& id, long long since) const;
    ToolResult cancel(const std::string& id) const;
    ToolResult wait(const std::string& id, int timeout_s) const;

    TaskService& tasks_;
    JobService& jobs_;
    CancellationToken fallback_token_;
};

std::string TasksTool::description() const noexcept {
    return "Inspect and control background work. Commands started with process_start, "
           "bash handoffs and watches all share one task inventory. Actions: list this "
           "window's tasks; status one task; output its bounded tail (page with since); "
           "cancel a running task; wait for it to finish. Output is bounded; the artifact "
           "path names the full transcript when one exists.";
}

json TasksTool::parameters_schema() const {
    json p = json::object();
    p["type"] = "object";
    json props = json::object();
    props["action"] =
        json::object({{"type", "string"},
                      {"enum", json::array({"list", "status", "output", "cancel", "wait"})},
                      {"description", "list | status | output | cancel | wait"}});
    props["id"] = json::object(
        {{"type", "string"}, {"description", "Task id (required for every action except list)."}});
    props["since"] = json::object(
        {{"type", "integer"},
         {"minimum", 0},
         {"description", "Byte offset into the task output; returns the next bounded chunk."}});
    props["timeout_s"] =
        json::object({{"type", "integer"},
                      {"minimum", 1},
                      {"maximum", 3600},
                      {"description", "Upper bound for wait, in seconds (default 60)."}});
    p["properties"] = props;
    p["required"] = json::array({"action"});
    return p;
}

bool TasksTool::requires_approval(const json& args) const noexcept {
    return args.value("action", std::string()) == "cancel";
}

std::string TasksTool::summarize(const json& args) const {
    const std::string action = args.value("action", std::string("list"));
    const std::string id = args.value("id", std::string());
    return id.empty() ? "tasks " + action : "tasks " + action + " " + id;
}

ToolResult TasksTool::execute(const json& args) const {
    const std::string action = args.value("action", std::string());
    if (action == "list")
        return list();
    ToolResult missing;
    const std::string id = require_id(args, action.empty() ? "action" : action.c_str(), missing);
    if (id.empty())
        return missing;
    if (action == "status")
        return status(id);
    if (action == "output")
        return output(id, read_int(args, "since", 0));
    if (action == "cancel")
        return cancel(id);
    if (action == "wait")
        return wait(id, bounded_timeout(read_int(args, "timeout_s", 60)));
    return {false, "", "tasks: action must be list, status, output, cancel or wait"};
}

std::optional<TaskRecord> TasksTool::owned(const std::string& id) const {
    auto record = tasks_.get(id);
    if (!record || record->owner.owner_id != run_owner_id())
        return std::nullopt;
    return record;
}

const CancellationToken& TasksTool::token() const {
    const RunScopeChain* scope = current_run_scope();
    if (scope && scope->leaf.cancel_token)
        return *scope->leaf.cancel_token;
    return fallback_token_;
}

ToolResult TasksTool::list() const {
    std::vector<TaskRecord> records = tasks_.list(run_owner_id());
    std::sort(records.begin(), records.end(), [](const TaskRecord& a, const TaskRecord& b) {
        if (a.created_ms != b.created_ms)
            return a.created_ms < b.created_ms;
        return a.id < b.id;
    });
    if (records.empty())
        return {true, "(no tasks)", "", {{"count", 0}}};
    std::vector<std::string> lines;
    lines.reserve(records.size());
    for (const auto& record : records)
        lines.push_back(format_task_row(record));
    return {true, join(lines), "", {{"count", records.size()}}};
}

ToolResult TasksTool::status(const std::string& id) const {
    auto record = owned(id);
    if (!record)
        return {false, "", "tasks status: no such task " + id};
    return {true, format_task_detail(*record), "", {{"id", id}, {"state", state_meta(*record)}}};
}

std::string TasksTool::output_body(const TaskRecord& record) const {
    std::string body;
    if (record.kind == TaskKind::Command) {
        const std::string backend = tasks_.backend_id(record.id);
        if (!backend.empty())
            body = jobs_.output(backend);
    }
    return body.empty() ? record.result.output_tail : body;
}

ToolResult TasksTool::output(const std::string& id, long long since) const {
    auto record = owned(id);
    if (!record)
        return {false, "", "tasks output: no such task " + id};

    const std::string body = output_body(*record);
    const long long clamped = since > 0 ? since : 0;
    const std::size_t start = clamped > static_cast<long long>(body.size())
                                  ? body.size()
                                  : static_cast<std::size_t>(clamped);
    const std::string chunk = body.substr(start, kOutputCap);
    const bool more = start + chunk.size() < body.size();

    std::ostringstream out;
    out << "[task " << id << " " << task_state_name(record->state);
    if (!body.empty())
        out << " | bytes " << start << ".." << start + chunk.size() << " of " << body.size();
    out << "]\n";
    if (chunk.empty())
        out << (more ? "(no new output)\n" : "(no output)\n");
    else {
        out << chunk;
        if (chunk.back() != '\n')
            out << "\n";
    }
    if (more)
        out << "[more: pass since=" << start + chunk.size() << "]\n";
    if (!record->result.artifact_path.empty())
        out << "artifact: " << record->result.artifact_path << "\n";

    json meta = {{"id", id},
                 {"state", state_meta(*record)},
                 {"next_since", start + chunk.size()},
                 {"truncated", more}};
    if (!record->result.artifact_path.empty())
        meta["artifact_path"] = record->result.artifact_path;
    return {true, out.str(), "", std::move(meta)};
}

ToolResult TasksTool::cancel(const std::string& id) const {
    auto record = owned(id);
    if (!record)
        return {false, "", "tasks cancel: no such task " + id};
    if (task_state_terminal(record->state)) {
        return {false, "", "tasks cancel: " + id + " is already " + task_state_name(record->state)};
    }
    if (!tasks_.cancel(id, "cancelled by tasks tool", token())) {
        return {false, "", "tasks cancel: " + id + " could not be cancelled"};
    }
    auto final = tasks_.get(id);
    if (!final)
        return {false, "", "tasks cancel: " + id + " disappeared"};
    const std::string detail = format_task_detail(*final);
    if (final->state != TaskState::Cancelled) {
        return {false, detail,
                "tasks cancel: " + id + " is " + task_state_name(final->state) +
                    "; the backend could not confirm the stop"};
    }
    return {true, detail, "", {{"id", id}, {"state", state_meta(*final)}}};
}

ToolResult TasksTool::wait(const std::string& id, int timeout_s) const {
    auto record = owned(id);
    if (!record)
        return {false, "", "tasks wait: no such task " + id};
    if (task_state_terminal(record->state)) {
        return {
            true, format_task_detail(*record), "", {{"id", id}, {"state", state_meta(*record)}}};
    }
    auto final = tasks_.wait_for_terminal(id, timeout_s * 1000, token());
    if (!final) {
        if (token().is_requested())
            return {false, "", "tasks wait: cancelled"};
        return {true,
                "task " + id + " is still running after " + std::to_string(timeout_s) + "s",
                "",
                {{"id", id}, {"state", "running"}}};
    }
    return {true, format_task_detail(*final), "", {{"id", id}, {"state", state_meta(*final)}}};
}

std::unique_ptr<Tool> make_tasks_tool(TaskService& tasks, JobService& jobs,
                                      const CancellationToken& fallback_token) {
    return std::make_unique<TasksTool>(tasks, jobs, fallback_token);
}

} // namespace agent
