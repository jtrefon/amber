#include "watch_plugin.h"

#include "watch_tools.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <filesystem>
#include <fstream>

namespace agent::plugins {

WatchPlugin::WatchPlugin(std::shared_ptr<CommandRunner> runner,
                         std::shared_ptr<ObserverModel> observer, std::string state_dir)
    : injected_runner_(std::move(runner)), injected_observer_(std::move(observer)),
      state_dir_(std::move(state_dir)) {}

bool WatchPlugin::initialize(const agent::PluginContext& ctx) {
    tools_ = &ctx.tools;
    // Resolve the fixed child against the canonical config root; a path that
    // somehow escapes that root is ignored rather than opened.
    const std::filesystem::path config =
        std::filesystem::weakly_canonical(agent::global_config_dir());
    const std::filesystem::path settings =
        std::filesystem::weakly_canonical(config / "plugins" / "watch" / "watch.conf");
    const std::string prefix = config.generic_string() + "/";
    if (settings.generic_string().rfind(prefix, 0) == 0)
        settings_ = load_watch_settings(settings.string());
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

std::map<std::string, agent::UsageTotals> WatchPlugin::take_usage() {
    return service_ ? service_->take_usage() : std::map<std::string, agent::UsageTotals>{};
}

std::string WatchPlugin::show_text(const std::string& id) const {
    if (!service_)
        return "watch plugin is inactive";
    auto record = service_->get(id);
    return record ? format_watch_record(*record) : "no watch " + id;
}

std::string WatchPlugin::task_id(const std::string& watch_id) const {
    return service_ ? service_->task_id(watch_id) : std::string();
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
    if (services.host)
        service_->set_tasks(services.host->tasks);
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
    caps.push_back(make_panel_capability());
    caps.push_back(make_settings_capability());
    return caps;
}

} // namespace agent::plugins
