#ifndef AMBER_TUI_FEED_MANAGER_H
#define AMBER_TUI_FEED_MANAGER_H

#include <map>
#include <set>
#include <string>

#include "nlohmann/json.hpp"

namespace tui {
class Tui;

class FeedManager {
public:
    explicit FeedManager(Tui& tui);
    void refresh_model_list();
    void refresh_policy_feed();
    void refresh_provider_feed();
    void refresh_job_feed();
    void refresh_plugin_feed();

private:
    // The policy-rule feed's three stages: gather the per-scope help text from
    // every window's agent, union that with the tool names and the curated
    // destructive patterns, then emit one leaf per tool.
    std::map<std::string, std::string> collect_rule_help() const;
    std::set<std::string>
    collect_policy_tools(const std::map<std::string, std::string>& rule_help) const;
    void add_policy_leaf(nlohmann::json& subtree, const std::string& tool,
                         const std::map<std::string, std::string>& rule_help);

    Tui& tui_;
};

} // namespace tui

#endif
