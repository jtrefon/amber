#include "watch_tools.h"

#include "agent/run_scope.h"
#include "agent/tool.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace agent::plugins {

namespace {

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

std::string owner_id() {
    const RunScopeChain* scope = current_run_scope();
    if (scope && scope->leaf.owner_id && !scope->leaf.owner_id->empty())
        return *scope->leaf.owner_id;
    return "default";
}

bool observe_enabled(const json& a) {
    const std::string mode = a.value("observe", std::string("auto"));
    return mode != "off" && mode != "false" && mode != "0";
}

std::string format_list(const std::vector<WatchRecord>& records) {
    if (records.empty())
        return "(no watches)";
    std::vector<std::string> lines;
    lines.reserve(records.size());
    std::transform(records.begin(), records.end(), std::back_inserter(lines),
                   [](const WatchRecord& record) { return format_watch_record(record); });
    return join(lines);
}

std::string status_text(const WatchRecord& record) {
    std::ostringstream out;
    out << record.id << " " << record.profile << " " << watch_state_name(record.state) << "\n"
        << "label: " << record.label << "\n"
        << "last: " << record.last_status;
    if (!record.journal.empty())
        out << "\njournal: " << record.journal.back();
    return out.str();
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
        return "Run a command, PR-check or URL watch in the background. Returns an id at once; "
               "the watch keeps a journal and produces a bounded report later. Use it for "
               "long builds, CI/PR checks, deploys and URL/process watches; use bash for quick "
               "commands and delegate (when available) for reasoning work.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"action",
               {{"type", "string"},
                {"enum", json::array({"start", "list", "status", "report", "cancel", "wait"})}}},
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
              {"observe", {{"type", "string"}, {"enum", json::array({"auto", "failure", "off"})}}},
              {"id", {{"type", "string"}}},
              {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
              {"detail", {{"type", "string"}, {"enum", json::array({"summary", "full"})}}}}},
            {"required", json::array({"action"})}};
    }

    bool requires_approval(const json& a) const noexcept override {
        return a.value("action", std::string()) == "start";
    }

    std::string summarize(const json& a) const override {
        if (a.value("action", std::string()) != "start")
            return "inspect background watches";
        const std::string profile = a.value("profile", std::string("process"));
        const std::string what = profile == "pr"
                                     ? "PR " + a.value("pr", std::string())
                                     : (profile == "url" ? a.value("url", std::string())
                                                         : a.value("command", std::string()));
        return "start a " + profile + " watch: " + what;
    }

    ToolResult execute(const json& a) const override {
        const std::string action = a.value("action", std::string());
        if (action == "start")
            return start(a);
        if (action == "list")
            return list();
        if (action == "status")
            return status(a);
        if (action == "report")
            return report(a);
        if (action == "cancel")
            return cancel(a);
        if (action == "wait")
            return wait(a);
        return {false, "", "watch: action must be start, list, status, report, cancel or wait"};
    }

private:
    ToolResult start(const json& a) const {
        WatchStart options;
        options.owner_id = owner_id();
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
        return {true,
                "started " + id + " (" + options.profile + "): " + options.label + "\n" +
                    "Use watch status/report " + id + ".",
                "",
                {{"id", id}, {"profile", options.profile}}};
    }

    ToolResult list() const {
        const auto records = service_.list();
        return {true, format_list(records), "", {{"count", records.size()}}};
    }

    ToolResult status(const json& a) const {
        const std::string id = a.value("id", std::string());
        if (id.empty())
            return {false, "", "watch status: missing id"};
        auto record = service_.get(id);
        if (!record)
            return {false, "", "watch status: unknown id " + id};
        return {true,
                status_text(*record),
                "",
                {{"id", id}, {"state", watch_state_name(record->state)}}};
    }

    ToolResult report(const json& a) const {
        const std::string id = a.value("id", std::string());
        if (id.empty())
            return {false, "", "watch report: missing id"};
        auto report = service_.report(id);
        if (!report)
            return {false, "", "watch report: no report yet for " + id};
        const bool full = a.value("detail", std::string("summary")) == "full";
        return {true,
                format_watch_report(*report, full),
                "",
                {{"id", id}, {"state", watch_state_name(report->state)}}};
    }

    ToolResult cancel(const json& a) const {
        const std::string id = a.value("id", std::string());
        if (id.empty())
            return {false, "", "watch cancel: missing id"};
        if (!service_.cancel(id))
            return {false, "", "watch cancel: unknown id " + id};
        return {true, "cancel requested for " + id, "", json::object()};
    }

    ToolResult wait(const json& a) const {
        const std::string id = a.value("id", std::string());
        if (id.empty())
            return {false, "", "watch wait: missing id"};
        const int timeout = std::max(1, std::min(a.value("timeout_s", 60), 3600));
        auto record = service_.wait(id, timeout);
        if (!record)
            return {false, "", "watch wait: unknown id " + id};
        return {true,
                status_text(*record),
                "",
                {{"id", id}, {"state", watch_state_name(record->state)}}};
    }

    WatchService& service_;
};

std::unique_ptr<agent::Tool> make_watch_tool(WatchService& service) {
    return std::make_unique<WatchTool>(service);
}

} // namespace agent::plugins
