#include "feed_manager.h"
#include "plugin_feed.h"
#include "tui.h"

#include <agent/job.h>
#include <agent/plugin_runtime.h>
#include <agent/model_probe.h>
#include <agent/policy.h>
#include <agent/shell_classify.h>

#include <algorithm>
#include <map>
#include <set>

namespace tui {
namespace {
const char* job_state_name(agent::JobState s) {
    switch (s) {
        case agent::JobState::Starting: return "starting";
        case agent::JobState::Running: return "running";
        case agent::JobState::Done: return "done";
        case agent::JobState::Killed: return "killed";
        case agent::JobState::Failed: return "failed";
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
        leaf["help"] = name == tui_.cfg_.provider_name ? "active provider" : "switch to this provider";
        tui_.register_action(action, [this, name](const std::string&) { tui_.cmd_provider(name); });
    }
    tui_.settings_.merge_completions_json(subtree);
}

void FeedManager::refresh_policy_feed() {
    std::map<std::string, std::string> rule_help;
    for (auto& w : tui_.window_manager_->all()) {
        if (!w->agent) continue;
        for (const auto& r : w->agent->policy().rules()) {
            if (r.level == agent::PolicyLevel::Ask) continue;
            std::string info = agent::policy_level_name(r.level);
            if (r.count > 0) info += " (used " + std::to_string(r.count) + "x)";
            rule_help[agent::scope_display(r)] = info;
        }
    }
    std::set<std::string> tools;
    for (const auto& t : tui_.reg_.snapshot_tools()) tools.insert(t->name());
    for (const auto& [tool, _] : rule_help) tools.insert(tool);
    // The curated destructive-command patterns are configurable rules too:
    // expose them as "bash.rm", "bash.git reset", ... so /set policy rule can
    // raise or lower them without hunting for the scope id.
    for (const auto& pat : agent::destructive_command_patterns()) {
        std::string display = "bash." + pat;
        std::replace(display.begin(), display.end(), ':', '.');
        tools.insert(display);
    }
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& tool : tools) {
        std::string info = rule_help.count(tool) ? rule_help.at(tool) : "no rule (ask)";
        std::string action = "core.config.set.policy.rule." + tool;
        nlohmann::json& leaf = subtree["set"]["children"]["policy"]["children"]["rule"]["children"][tool];
        leaf["action"] = action;
        leaf["help"] = info;
        tui_.register_action(action, [this, tool](const std::string& a) { tui_.apply_policy_rule(tool, a); });
        if (rule_help.count(tool)) {
            std::string gaction = "core.config.get.policy.rule." + tool;
            nlohmann::json& g = subtree["get"]["children"]["policy"]["children"]["rule"]["children"][tool];
            g["action"] = gaction;
            g["help"] = info;
            tui_.register_action(gaction, [this, tool](const std::string&) { tui_.show_policy_rule(tool); });
        }
    }
    tui_.settings_.merge_completions_json(subtree);
}

void FeedManager::refresh_model_list() {
    tui_.slash_dispatcher_->set_model_info(agent::list_model_info(tui_.cfg_));
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& m : tui_.slash_dispatcher_->model_info()) {
        std::string id = m.id;
        nlohmann::json& leaf = subtree["set"]["children"]["model"]["children"][id];
        leaf["action"] = "core.config.set.model." + id;
        int ctx = m.context ? m.context : m.context_train;
        if (ctx > 0) leaf["help"] = "ctx " + std::to_string(ctx);
        tui_.register_action(leaf["action"].get<std::string>(),
                             [this, id](const std::string&) { tui_.cmd_model_set(id); });
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
        tui_.register_action("core.job.kill." + id, [this, id](const std::string&) { tui_.job_kill(id); });
        tui_.register_action("core.job.read." + id, [this, id](const std::string&) { tui_.job_read(id); });
    }
    tui_.settings_.merge_completions_json(subtree);
}

} // namespace tui
