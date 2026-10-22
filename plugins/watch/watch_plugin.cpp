#include "watch_plugin.h"

#include "watch_tools.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

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

std::string root_usage() {
    return "usage: /plugin watch list | show <id> | report <id> | stop <id> | wait <id>";
}

} // namespace

WatchPlugin::WatchPlugin(std::shared_ptr<CommandRunner> runner,
                         std::shared_ptr<ObserverModel> observer, std::string state_dir)
    : injected_runner_(std::move(runner)), injected_observer_(std::move(observer)),
      state_dir_(std::move(state_dir)) {}

bool WatchPlugin::initialize(const agent::PluginContext& ctx) {
    tools_ = &ctx.tools;
    // codeql[cpp/path-injection] lgtm[cpp/path-injection] — fixed watch.conf under the
    // user's own sanitized XDG config root, not a caller-supplied path.
    settings_ = load_watch_settings(agent::global_config_dir() + "/plugins/watch/watch.conf");
    return true;
}

void WatchPlugin::shutdown() {
    if (service_) {
        service_->shutdown();
        service_.reset();
    }
}

void WatchPlugin::cancel_owner(const std::string& owner_id) {
    if (service_)
        service_->cancel_owner(owner_id);
}

void WatchPlugin::abandon_owner(const std::string& owner_id) {
    if (service_)
        service_->abandon_owner(owner_id);
}

void WatchPlugin::ack(const std::string& watch_id) {
    if (service_)
        service_->ack(watch_id);
}

void WatchPlugin::reemit_pending() {
    if (service_)
        service_->reemit_pending();
}

std::vector<WatchReport> WatchPlugin::pending_reports() const {
    return service_ ? service_->pending_reports() : std::vector<WatchReport>{};
}

std::vector<WatchRecord> WatchPlugin::list_records() const {
    return service_ ? service_->list() : std::vector<WatchRecord>{};
}

std::string WatchPlugin::show_text(const std::string& id) const {
    return service_ ? command_text("show", id) : "watch plugin is inactive";
}

bool WatchPlugin::stop_watch(const std::string& id) {
    return service_ && service_->cancel(id);
}

int WatchPlugin::running_count() const {
    return service_ ? service_->running_count() : 0;
}

WatchService* WatchPlugin::ensure_service(agent::PluginServices& services) {
    if (service_)
        return service_.get();

    std::shared_ptr<CommandRunner> runner = injected_runner_;
    if (!runner && services.host && services.host->jobs)
        runner = std::make_shared<JobCommandRunner>(*services.host->jobs);
    if (!runner)
        return nullptr;

    const std::string prompt = load_observer_prompt();
    std::shared_ptr<ObserverModel> observer = injected_observer_;
    if (!observer && settings_.observer && !prompt.empty() && services.config)
        observer = std::make_shared<HttpObserverModel>(services.config);

    std::string root = state_dir_;
    if (root.empty()) {
        const std::string state = agent::Workspace::state_dir();
        root = state.empty() ? "watch" : state + "/watch";
    }
    service_ =
        std::make_unique<WatchService>(std::move(runner), std::move(observer), WatchStore(root),
                                       prompt, &services.events(), settings_);
    return service_.get();
}

std::string WatchPlugin::policy_block() const {
    // No service means the tool capability declined (a bare host with no job
    // service); advertising a tool the agent cannot call is worse than silence.
    if (!service_)
        return {};
    std::string out = load_optional_prompt("prompts/watch/system.md");
    if (out.empty())
        return out;
    if (tools_ && tools_->find("task")) {
        const std::string delegation = load_optional_prompt("prompts/watch/delegation.md");
        if (!delegation.empty())
            out += "\n\n" + delegation;
    }
    return out;
}

std::string WatchPlugin::command_text(const std::string& verb, const std::string& arg) const {
    if (!service_)
        return "watch plugin is not active (no job service)";
    if (verb.empty() || verb == "list") {
        auto records = service_->list();
        if (records.empty())
            return "(no watches)\n" + root_usage();
        std::vector<std::string> lines;
        lines.reserve(records.size());
        std::transform(records.begin(), records.end(), std::back_inserter(lines),
                       [](const WatchRecord& record) { return format_watch_record(record); });
        return join(lines);
    }
    if (arg.empty())
        return "usage: /watch " + verb + " <id>";
    if (verb == "show") {
        auto record = service_->get(arg);
        return record ? format_watch_record(*record) : "no watch " + arg;
    }
    if (verb == "report") {
        auto report = service_->report(arg);
        return report ? format_watch_report(*report, true) : "no report yet for " + arg;
    }
    if (verb == "cancel")
        return service_->cancel(arg) ? "cancel requested for " + arg : "no watch " + arg;
    if (verb == "wait")
        return "use the watch tool's wait action; the command surface does not block the UI";
    return root_usage();
}

std::unique_ptr<agent::Capability> WatchPlugin::make_tool_capability() {
    return std::make_unique<agent::ToolCapability>(
        "watch_tool",
        [this](agent::PluginServices& services) -> std::vector<std::unique_ptr<agent::Tool>> {
            WatchService* service = ensure_service(services);
            if (!service)
                return {};
            return agent::wrap_tool(make_watch_tool(*service));
        },
        agent::ToolCapability::Meta{{{"watch", {"watching", agent::ToolRole::Control}}}});
}

std::unique_ptr<agent::Capability> WatchPlugin::make_policy_capability() {
    return std::make_unique<agent::PromptBlockCapability>(
        "watch_policy", 600, [this] { return policy_block(); }, agent::PromptPlacement::System);
}

std::unique_ptr<agent::Capability> WatchPlugin::make_status_capability() {
    return std::make_unique<agent::StatusSegmentCapability>(
        "watch", 600, 8, [this](const agent::StatusSnapshot&) -> agent::StatusText {
            if (!service_)
                return {};
            const WatchCounts counts = service_->counts();
            if (counts.running == 0 && counts.attention == 0 && counts.observers == 0)
                return {};
            std::string text = "[w" + std::to_string(counts.running);
            if (counts.attention > 0)
                text += "!";
            if (counts.observers > 0)
                text += "+o" + std::to_string(counts.observers);
            text += "]";
            return {std::move(text),
                    counts.attention > 0 ? agent::StatusTone::Warn : agent::StatusTone::Dim};
        });
}

std::unique_ptr<agent::Capability> WatchPlugin::make_panel_capability() {
    agent::PanelSpec spec;
    spec.id = "watch";
    spec.title = "Watches";
    // Read-only in v1: PanelSpec has no selected-row handoff, so cancel/report
    // stay on /watch. The table follows the same store as the commands.
    spec.lines = [this](int) {
        std::vector<std::string> lines;
        if (!service_) {
            lines.emplace_back("watch plugin is inactive");
            return lines;
        }
        lines.emplace_back("id                          profile  state       last");
        auto records = service_->list();
        if (records.empty())
            lines.emplace_back("(no watches)");
        lines.reserve(records.size() + 1);
        for (const auto& record : records) {
            std::string line = record.id;
            if (line.size() < 27)
                line.resize(27, ' ');
            else
                line.resize(27);
            std::string profile = record.profile;
            profile.resize(8, ' ');
            std::string state = watch_state_name(record.state);
            state.resize(11, ' ');
            line += " ";
            line += profile;
            line += " ";
            line += state;
            line += " ";
            line += record.last_status;
            lines.push_back(std::move(line));
        }
        return lines;
    };
    return std::make_unique<agent::PanelCapability>(std::move(spec));
}

std::unique_ptr<agent::Capability> WatchPlugin::make_command_capability() const {
    agent::CommandSpec spec;
    spec.name = "watch"; // legacy field; the host mounts at /plugin/watch
    spec.help = "Background watches and their reports.";
    spec.man = "Usage: /plugin watch list | show <id> | report <id> | stop <id> | wait <id>. "
               "The model starts watches with the watch tool; these commands inspect and "
               "manage them.";
    spec.subtree = nlohmann::json::parse(R"({
        "list": {"help": "List watches"},
        "show": {"help": "Show one watch"},
        "report": {"help": "Show a watch report"},
        "stop": {"help": "Stop a watch", "aliases": ["cancel"]},
        "wait": {"help": "Wait for a watch to finish"}
    })");
    spec.handlers[""] = [this](const std::string&) { return command_text("", ""); };
    spec.handlers["list"] = [this](const std::string&) { return command_text("list", ""); };
    spec.handlers["show"] = [this](const std::string& arg) { return command_text("show", arg); };
    spec.handlers["report"] = [this](const std::string& arg) {
        return command_text("report", arg);
    };
    spec.handlers["stop"] = [this](const std::string& arg) { return command_text("cancel", arg); };
    spec.handlers["wait"] = [this](const std::string& arg) { return command_text("wait", arg); };
    return std::make_unique<agent::CommandCapability>(std::move(spec));
}

std::unique_ptr<agent::Capability> WatchPlugin::make_settings_capability() {
    const std::string path = agent::global_config_dir() + "/plugins/watch/watch.conf";
    std::vector<agent::PluginSettingSpec> specs;
    auto add_int = [&](const std::string& key, const std::string& help, int WatchSettings::*field) {
        agent::PluginSettingSpec setting;
        setting.key = key;
        setting.help = help;
        setting.type = "int";
        setting.get = [this, field] { return std::to_string(settings_.*field); };
        setting.set = [this, path, key](const std::string& value) {
            if (!apply_watch_setting(settings_, key, value))
                return false;
            save_watch_settings(path, settings_);
            return true;
        };
        specs.push_back(std::move(setting));
    };
    agent::PluginSettingSpec observer;
    observer.key = "observer";
    observer.help = "Run the one-shot read-only observer after a watch transition.";
    observer.type = "choice";
    observer.choices = {"on", "off"};
    observer.get = [this] { return std::string(settings_.observer ? "on" : "off"); };
    observer.set = [this, path](const std::string& value) {
        if (!apply_watch_setting(settings_, "observer", value))
            return false;
        save_watch_settings(path, settings_);
        return true;
    };
    specs.push_back(std::move(observer));
    add_int("max_running", "Maximum concurrently running watches.", &WatchSettings::max_running);
    add_int("interval_s", "Default poll interval in seconds.", &WatchSettings::interval_s);
    add_int("deadline_s", "Default watch deadline in seconds.", &WatchSettings::deadline_s);
    add_int("retention", "Maximum retained watch records.", &WatchSettings::retention);
    add_int("retention_days", "Retain watch records for this many days.",
            &WatchSettings::retention_days);
    return std::make_unique<agent::SettingsCapability>(std::move(specs));
}

std::vector<std::unique_ptr<agent::Capability>> WatchPlugin::capabilities() {
    std::vector<std::unique_ptr<agent::Capability>> caps;
    caps.push_back(make_tool_capability());
    caps.push_back(agent::make_tool_doc_capability("watch_doc", agent::tool_doc_priority::kWatch,
                                                   "prompts/tools/watch.md"));
    caps.push_back(make_policy_capability());
    caps.push_back(make_status_capability());
    caps.push_back(make_panel_capability());
    caps.push_back(make_settings_capability());
    caps.push_back(make_command_capability());
    return caps;
}

} // namespace agent::plugins
