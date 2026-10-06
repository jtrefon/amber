#include "feed_manager.h"
#include "plugin_feed.h"
#include "tui.h"

#include <agent/job.h>
#include <agent/plugin_runtime.h>
#include <agent/model_probe.h>
#include <agent/policy.h>
#include <agent/providers.h>
#include <agent/shell_classify.h>

#include "tui/model_picker.h"

#include <algorithm>
#include <map>
#include <set>

namespace tui {
namespace {
const char* job_state_name(agent::JobState s) {
    switch (s) {
    case agent::JobState::Starting:
        return "starting";
    case agent::JobState::Running:
        return "running";
    case agent::JobState::Done:
        return "done";
    case agent::JobState::Killed:
        return "killed";
    case agent::JobState::Failed:
        return "failed";
    }
    return "?";
}
} // namespace

FeedManager::FeedManager(Tui& tui) : tui_(tui) {}

void FeedManager::refresh_provider_feed() {
    nlohmann::json subtree = nlohmann::json::object();
    auto& leaves = subtree["set"]["children"]["provider"]["children"];
    for (const auto& p : tui_.providers_->available()) {
        const std::string& name = p.name;
        const std::string action = "core.config.set.provider." + name;
        nlohmann::json& leaf = leaves[name];
        leaf["action"] = action;
        leaf["help"] =
            name == tui_.cfg_.provider_name ? "active provider" : "switch to this provider";
        tui_.register_action(action, [this, name](const std::string&) { tui_.cmd_provider(name); });
    }
    tui_.settings_.merge_completions_json(subtree);
}

// The per-scope help text ("ask" / "used Nx"), gathered from every window's
// agent; the first window to describe a scope wins.
std::map<std::string, std::string> FeedManager::collect_rule_help() const {
    std::map<std::string, std::string> rule_help;
    for (auto& w : tui_.window_manager_->all()) {
        if (!w->agent)
            continue;
        for (const auto& r : w->agent->policy().rules()) {
            if (r.level == agent::PolicyLevel::Ask)
                continue;
            std::string info = agent::policy_level_name(r.level);
            if (r.count > 0)
                info += " (used " + std::to_string(r.count) + "x)";
            rule_help[agent::scope_display(r)] = info;
        }
    }
    return rule_help;
}

// Every tool a rule can be set on: the registry's tools, the scopes that
// already carry a rule, and the curated destructive-command patterns (exposed
// as "bash.rm", "bash.git reset", ... so /set policy rule can raise or lower
// them without hunting for the scope id).
std::set<std::string>
FeedManager::collect_policy_tools(const std::map<std::string, std::string>& rule_help) const {
    std::set<std::string> tools;
    for (const auto& t : tui_.reg_.snapshot_tools())
        tools.insert(t->name());
    for (const auto& [tool, _] : rule_help)
        tools.insert(tool);
    for (const auto& pat : agent::destructive_command_patterns()) {
        std::string display = "bash." + pat;
        std::replace(display.begin(), display.end(), ':', '.');
        tools.insert(display);
    }
    return tools;
}

// One tool's set leaf, plus a get leaf when a rule already exists.
void FeedManager::add_policy_leaf(nlohmann::json& subtree, const std::string& tool,
                                  const std::map<std::string, std::string>& rule_help) {
    const std::string info = rule_help.count(tool) ? rule_help.at(tool) : "no rule (ask)";
    const std::string action = "core.config.set.policy.rule." + tool;
    nlohmann::json& leaf =
        subtree["set"]["children"]["policy"]["children"]["rule"]["children"][tool];
    leaf["action"] = action;
    leaf["help"] = info;
    tui_.register_action(action,
                         [this, tool](const std::string& a) { tui_.apply_policy_rule(tool, a); });
    if (!rule_help.count(tool))
        return;
    const std::string gaction = "core.config.get.policy.rule." + tool;
    nlohmann::json& g = subtree["get"]["children"]["policy"]["children"]["rule"]["children"][tool];
    g["action"] = gaction;
    g["help"] = info;
    tui_.register_action(gaction,
                         [this, tool](const std::string&) { tui_.show_policy_rule(tool); });
}

void FeedManager::refresh_policy_feed() {
    const std::map<std::string, std::string> rule_help = collect_rule_help();
    const std::set<std::string> tools = collect_policy_tools(rule_help);
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& tool : tools)
        add_policy_leaf(subtree, tool, rule_help);
    tui_.settings_.merge_completions_json(subtree);
}

namespace {

// One provider's cached catalogue. Cache-only: never a network call on the UI
// thread. An empty result means that provider contributes nothing yet, which is
// the same thing as an empty tab.
std::vector<ProviderModel> cached_models_for(const ProviderEndpoint& e) {
    agent::Config cfg;
    cfg.api_base = e.api_base;
    cfg.flavor = e.flavor;
    std::vector<ProviderModel> out;
    for (const auto& m : agent::list_model_info_cached(cfg)) {
        ProviderModel pm;
        pm.provider = e.name;
        pm.id = m.id;
        pm.context = m.context ? m.context : m.context_train;
        out.push_back(pm);
    }
    return out;
}

// The endpoints worth reading: every known provider, configured or not, so
// catalogs_from() can apply the "has an endpoint" rule in one tested place.
std::vector<ProviderEndpoint> endpoints_of(const agent::ProviderService& providers) {
    std::vector<ProviderEndpoint> out;
    for (const auto& p : providers.available()) {
        ProviderEndpoint e;
        e.name = p.name;
        e.api_base = p.api_base;
        e.flavor = p.flavor;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

void FeedManager::refresh_model_list() {
    tui_.slash_dispatcher_->set_model_info(agent::list_model_info_cached(tui_.cfg_));
    const auto rows = tui_.providers_
                          ? aggregate_provider_models(
                                catalogs_from(endpoints_of(*tui_.providers_), &cached_models_for),
                                tui_.cfg_.provider_name)
                          : std::vector<ProviderModel>{};
    const auto subtree = model_subtree(rows, "core.config.set.model.");
    for (const auto& m : rows) {
        const std::string key = provider_key(m.provider, m.id);
        tui_.register_action(
            subtree["set"]["children"]["model"]["children"][key]["action"].get<std::string>(),
            [this, provider = m.provider, id = m.id](const std::string&) {
                tui_.slash_dispatcher_->cmd_model_set_for(provider, id);
            });
    }
    tui_.settings_.merge_completions_json(subtree);
}

// plugin control leaves: /set plugin on|off <id> toggles, /get plugin info <id>
// reports. Ids hang under their verb, so the drawers of get.plugin and
// set.plugin stay command lists whatever the plugin count. Regenerated on every
// change so the tree always matches the runtime's actual state.
void FeedManager::refresh_plugin_feed() {
    const auto plugins = tui_.plugin_runtime_.list();
    std::vector<PluginFeedEntry> entries;
    entries.reserve(plugins.size());
    for (const auto& p : plugins)
        entries.push_back({p.id, p.version, p.tier, p.enabled});
    tui_.settings_.merge_completions_json(plugin_feed_subtree(entries));

    for (const auto& p : plugins) {
        const std::string id = p.id;
        tui_.register_action(plugin_action("get.plugin.info", id),
                             [this, id](const std::string&) { tui_.show_plugin(id); });
        // set_plugin rebuilds the command tree (and with it this feed), so the
        // rows always match the state the toggle left behind.
        tui_.register_action(plugin_action("set.plugin.on", id),
                             [this, id](const std::string&) { tui_.set_plugin(id, true); });
        tui_.register_action(plugin_action("set.plugin.off", id),
                             [this, id](const std::string&) { tui_.set_plugin(id, false); });
    }
}

void FeedManager::refresh_job_feed() {
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& j : tui_.jobs_.list()) {
        std::string id = j.id;
        nlohmann::json& kill_leaf = subtree["job"]["children"]["kill"]["children"][id];
        kill_leaf["action"] = "core.job.kill." + id;
        kill_leaf["help"] = tui::job_state_name(j.state);
        nlohmann::json& read_leaf = subtree["job"]["children"]["read"]["children"][id];
        read_leaf["action"] = "core.job.read." + id;
        read_leaf["help"] = tui::job_state_name(j.state);
        tui_.register_action("core.job.kill." + id,
                             [this, id](const std::string&) { tui_.job_kill(id); });
        tui_.register_action("core.job.read." + id,
                             [this, id](const std::string&) { tui_.job_read(id); });
    }
    tui_.settings_.merge_completions_json(subtree);
}

} // namespace tui
