#include "feed_manager.h"
#include "tui.h"

#include <agent/job.h>
#include <agent/model_probe.h>
#include <agent/policy.h>

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
            rule_help[r.tool] = info;
        }
    }
    std::set<std::string> tools;
    for (const auto& t : tui_.reg_.snapshot_tools()) tools.insert(t->name());
    for (const auto& [tool, _] : rule_help) tools.insert(tool);
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
    tui_.model_info_ = agent::list_model_info(tui_.cfg_);
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& m : tui_.model_info_) {
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
