#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tui {

// One registered plugin, in the shape the command tree needs.
struct PluginFeedEntry {
    std::string id;
    std::string version;
    std::string tier;  // "bundled" or "external"
    bool enabled = false;
};

// The live plugin feed: the value leaves of /get plugin info <id> and
// /set plugin on|off <id>. The ids hang under their verb, so the drawers of
// get.plugin and set.plugin stay a command list however many plugins register.
// Pure data — the host registers the action closures and merges the subtree.
nlohmann::json plugin_feed_subtree(const std::vector<PluginFeedEntry>& plugins);

// The action of one verb leaf: plugin_action("get.plugin.info", "clock") →
// "core.config.get.plugin.info.clock". One naming source for the feed, the
// handlers, and the tests.
std::string plugin_action(const std::string& verb, const std::string& id);

} // namespace tui
