#include "watch_tools.h"

#include "agent/run_scope.h"
#include "agent/tool.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace agent::plugins {

namespace {

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

bool observe_enabled(const json& a) {
    const std::string mode = a.value("observe", std::string("auto"));
    return mode != "off" && mode != "false" && mode != "0";
}

} // namespace

std::string format_watch_record(const WatchRecord& record) {
    std::ostringstream out;
    out << record.id << " | " << record.profile << " | " << watch_state_name(record.state) << " | "
        << record.label;
    if (!record.last_status.empty())
        out << " | " << record.last_status;
    return out.str();
}

std::string format_watch_report(const WatchReport& report, bool full) {
    std::ostringstream out;
    out << report.headline << "\n";
    out << "state: " << watch_state_name(report.state) << "\n";
    for (const auto& line : report.observed) {
        out << "- " << (full ? line : clip(line, 500)) << "\n";
    }
    if (!report.likely_cause.empty())
        out << "likely cause: " << report.likely_cause << "\n";
    if (!report.next_action.empty())
        out << "next: " << report.next_action << "\n";
    if (!report.confidence.empty())
        out << "confidence: " << report.confidence << "\n";
    if (!report.artifact_path.empty())
        out << "artifact: " << report.artifact_path << "\n";
    if (report.format_error)
        out << "(observer output was not valid JSON; deterministic report kept)\n";
    return out.str();
}

class WatchTool final : public agent::Tool {
public:
    explicit WatchTool(WatchService& service) : service_(service) {}

    std::string name() const noexcept override { return "watch"; }

    std::string description() const noexcept override {
        return "Run a command, PR-check or URL watch in the background. Returns a task id at "
               "once; the watch keeps a journal and produces a bounded report later. Use it for "
               "long builds, CI/PR checks, deploys and URL/process watches. Inspect or stop it "
               "with tasks status/output/cancel/wait; use bash for quick commands and delegate "
               "(when available) for reasoning work.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"action", {{"type", "string"}, {"enum", json::array({"start"})}}},
              {"profile", {{"type", "string"}, {"enum", json::array({"process", "pr", "url"})}}},
              {"command", {{"type", "string"}}},
              {"pr", {{"type", "string"}}},
              {"url", {{"type", "string"}}},
              {"cwd", {{"type", "string"}}},
              {"repo", {{"type", "string"}}},
              {"label", {{"type", "string"}}},
              {"interval_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
              {"deadline_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400}}},
              {"success_exit", {{"type", "integer"}}},
              {"observe",
               {{"type", "string"}, {"enum", json::array({"auto", "failure", "off"})}}}}},
            {"required", json::array({"action"})}};
    }

    bool requires_approval(const json&) const noexcept override { return true; }

    std::string summarize(const json& a) const override {
        const std::string profile = a.value("profile", std::string("process"));
        const std::string what = profile == "pr"
                                     ? "PR " + a.value("pr", std::string())
                                     : (profile == "url" ? a.value("url", std::string())
                                                         : a.value("command", std::string()));
        return "start a " + profile + " watch: " + what;
    }

    ToolResult execute(const json& a) const override {
        if (a.value("action", std::string()) != "start")
            return {false, "", "watch: action must be start; use tasks to inspect a watch"};
        return start(a);
    }

private:
    ToolResult start(const json& a) const {
        WatchStart options;
        options.owner_id = run_owner_id();
        options.profile = a.value("profile", std::string("process"));
        options.command = a.value("command", std::string());
        options.pr = a.value("pr", std::string());
        options.url = a.value("url", std::string());
        options.cwd = a.value("cwd", a.value("repo", std::string()));
        // A watch started by a window belongs to that window's tree.
        if (options.cwd.empty()) {
            if (const std::string* root = scoped_workspace_root())
                options.cwd = *root;
        }
        options.label = a.value("label", std::string());
        options.interval_s = a.value("interval_s", 0);
        options.deadline_s = a.value("deadline_s", 0);
        options.success_exit = a.value("success_exit", 0);
        options.observer = observe_enabled(a);
        std::string error;
        const std::string id = service_.start(options, error);
        if (id.empty())
            return {false, "", error.empty() ? "watch: start failed" : error};
        const std::string task = service_.task_id(id);
        json meta = {{"id", id}, {"watch_id", id}, {"profile", options.profile}};
        if (!task.empty())
            meta["task_id"] = task;
        // A host without a task inventory still gets the watch id; the shipped
        // hosts wire TaskService, so the task id is the normal answer.
        return {true, task.empty() ? id : task, "", std::move(meta)};
    }

    WatchService& service_;
};

std::unique_ptr<agent::Tool> make_watch_tool(WatchService& service) {
    return std::make_unique<WatchTool>(service);
}

} // namespace agent::plugins
