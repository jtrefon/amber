
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/job.h"

#include <sstream>
#include <string>
#include "agent/json_read.h"
#include <unistd.h>

namespace agent {

namespace {

// Wait for the job to speak: return as soon as output arrives, or after
// `idle_s` seconds of silence, or when the job ends. Nothing is killed — the
// budget is inactivity, and whether to wait again stays the agent's call.
void wait_for_output(const std::shared_ptr<Job>& job, int idle_s) {
    long prev = job->info().seconds_since_output;
    long silent_ms = 0;
    while (!job->is_done() && silent_ms < idle_s * 1000L) {
        usleep(50 * 1000);
        silent_ms += 50;
        const long now = job->info().seconds_since_output;
        if (prev > 0 && now == 0)
            return; // it spoke: the counter reset
        prev = now;
    }
}

// Shared helper: render a one-line status prefix for a job snapshot so the
// model always knows whether a process is still alive.
std::string status_line(const JobInfo& i) {
    std::ostringstream s;
    s << "[job " << i.id << " ";
    switch (i.state) {
    case JobState::Running:
    case JobState::Starting:
        s << "running";
        break;
    case JobState::Done:
        s << "done exit " << i.exit_code;
        break;
    case JobState::Killed:
        s << "killed";
        break;
    case JobState::Failed:
        s << "failed to start";
        break;
    }
    if (i.remaining_idle_s >= 0)
        s << " idle " << i.remaining_idle_s << "s";
    if (i.remaining_hard_s >= 0)
        s << " hard " << i.remaining_hard_s << "s";
    s << "]";
    return s.str();
}

class ProcessStartTool : public Tool {
public:
    explicit ProcessStartTool(JobService& jobs) : jobs_(jobs) {}

    std::string name() const noexcept override { return "process_start"; }
    std::string description() const noexcept override {
        return "Start a shell command in the BACKGROUND and return a job id "
               "immediately (non-blocking). Use this for long-running commands "
               "such as servers, watchers, or builds (e.g. 'npm run dev'). The "
               "process keeps running across turns. Poll its output with "
               "process_read <id> and stop it with process_stop <id>. Arguments: "
               "command (required), timeout (hard seconds, default 600), "
               "idle_timeout (seconds of no output before auto-kill, default "
               "30), cwd (optional, defaults to workspace root).";
    }
    json parameters_schema() const override {
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
        p["properties"] = props;
        p["required"] = json::array({"command"});
        return p;
    }
    bool requires_approval(const json&) const noexcept override { return true; }
    bool is_read_only() const noexcept override { return false; }
    std::string summarize(const json& a) const override {
        std::string c = a.contains("command") && a["command"].is_string()
                            ? a["command"].get<std::string>()
                            : "";
        if (c.size() > 200) {
            c.resize(197);
            c += "...";
        }
        return "bg start: " + c;
    }
    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("command") || !a["command"].is_string() ||
            a["command"].get<std::string>().empty()) {
            r.ok = false;
            r.error = "missing 'command'";
            return r;
        }
        std::string cmd = a["command"].get<std::string>();
        long hard = static_cast<long>(read_int(a, "timeout", 600));
        long idle = static_cast<long>(read_int(a, "idle_timeout", 30));
        std::string cwd = read_string(a, "cwd", "");
        std::string id = jobs_.start(cmd, cwd, hard, idle);
        if (id.empty()) {
            r.ok = false;
            r.error = "failed to start background process";
            return r;
        }
        r.ok = true;
        r.meta = {{"job_id", id}};
        r.output = id;
        return r;
    }

private:
    JobService& jobs_;
};

class ProcessReadTool : public Tool {
public:
    explicit ProcessReadTool(JobService& jobs) : jobs_(jobs) {}

    std::string name() const noexcept override { return "process_read"; }
    std::string description() const noexcept override {
        return "Read new output from a background job started with "
               "process_start. Returns only the output produced since the "
               "previous call (delta), plus a status line. Poll this in a loop "
               "to follow a running process. Arguments: id (required), all "
               "(optional bool: return the full output instead of the delta), "
               "wait (optional int: seconds of silence to wait for before "
               "answering; returns early when the job speaks, kills nothing).";
    }
    json parameters_schema() const override {
        json p = json::object();
        p["type"] = "object";
        json props = json::object();
        props["id"] = json::object({{"type", "string"}, {"description", "Job id."}});
        props["all"] = json::object(
            {{"type", "boolean"},
             {"description", "Return full output instead of the delta (default false)."}});
        props["wait"] = json::object(
            {{"type", "integer"},
             {"description", "Seconds of silence to wait for before answering "
                             "(default 0: answer now). Returns early when the job "
                             "speaks. Nothing is killed."}});
        p["properties"] = props;
        p["required"] = json::array({"id"});
        return p;
    }
    bool requires_approval(const json&) const noexcept override { return false; }
    bool is_read_only() const noexcept override { return true; }
    std::string summarize(const json& a) const override {
        std::string id = a.contains("id") && a["id"].is_string() ? a["id"].get<std::string>() : "";
        return "bg read: " + id;
    }
    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("id") || !a["id"].is_string()) {
            r.ok = false;
            r.error = "missing 'id'";
            return r;
        }
        std::string id = a["id"].get<std::string>();
        std::shared_ptr<Job> job = jobs_.get(id);
        if (!job) {
            r.ok = false;
            r.error = "no such job: " + id;
            return r;
        }
        bool all = read_bool(a, "all", false);
        const auto wait_raw = read_int(a, "wait", 0);
        const int wait_s = static_cast<int>(wait_raw > 86400 ? 86400 : (wait_raw > 0 ? wait_raw : 0));
        if (wait_s > 0)
            wait_for_output(job, wait_s);
        // Read from the held reference (the service map entry may already be
        // gone via a concurrent stop).
        std::string body = all ? job->output() : job->read_delta();
        JobInfo info = job->info();
        std::ostringstream out;
        out << status_line(info) << "\n";
        if (body.empty())
            out << "(no new output)\n";
        else
            out << body;
        r.meta = {{"job_id", id},
                  {"state", static_cast<int>(info.state)},
                  {"delta", !body.empty()},
                  {"duration_ms", info.seconds_since_start * 1000}};
        r.ok = true;
        r.output = out.str();
        return r;
    }

private:
    JobService& jobs_;
};

class ProcessStopTool : public Tool {
public:
    explicit ProcessStopTool(JobService& jobs) : jobs_(jobs) {}

    std::string name() const noexcept override { return "process_stop"; }
    std::string description() const noexcept override {
        return "Stop a background job started with process_start, killing its "
               "whole process group, and return the output captured so far. "
               "Arguments: id (required).";
    }
    json parameters_schema() const override {
        json p = json::object();
        p["type"] = "object";
        json props = json::object();
        props["id"] = json::object({{"type", "string"}, {"description", "Job id."}});
        p["properties"] = props;
        p["required"] = json::array({"id"});
        return p;
    }
    bool requires_approval(const json&) const noexcept override { return true; }
    bool is_read_only() const noexcept override { return false; }
    std::string summarize(const json& a) const override {
        std::string id = a.contains("id") && a["id"].is_string() ? a["id"].get<std::string>() : "";
        return "bg stop: " + id;
    }
    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("id") || !a["id"].is_string()) {
            r.ok = false;
            r.error = "missing 'id'";
            return r;
        }
        std::string id = a["id"].get<std::string>();
        std::string tail = jobs_.output(id);
        if (!jobs_.stop(id)) {
            r.ok = false;
            r.error = "no such job: " + id;
            return r;
        }
        std::ostringstream out;
        out << "[job " << id << " stopped]\n";
        out << (tail.empty() ? "(no output captured)\n" : tail);
        r.meta = {{"job_id", id}};
        r.ok = true;
        r.output = out.str();
        return r;
    }

private:
    JobService& jobs_;
};

} // namespace

std::vector<std::unique_ptr<Tool>> make_process_tools(JobService& jobs) {
    std::vector<std::unique_ptr<Tool>> v;
    v.push_back(std::make_unique<ProcessStartTool>(jobs));
    v.push_back(std::make_unique<ProcessReadTool>(jobs));
    v.push_back(std::make_unique<ProcessStopTool>(jobs));
    return v;
}

} // namespace agent
