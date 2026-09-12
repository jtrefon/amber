#include "plugin_feed.h"

namespace tui {

namespace {

std::string version_note(const std::string& version) {
    return version.empty() ? "v?" : "v" + version;
}

// "on, bundled v0.4.0" — the state line every info leaf carries.
std::string state_note(const PluginFeedEntry& p) {
    return (p.enabled ? "on, " : "off, ") + p.tier + " " + version_note(p.version);
}

} // namespace

std::string plugin_action(const std::string& verb, const std::string& id) {
    return "core.config." + verb + "." + id;
}

nlohmann::json plugin_feed_subtree(const std::vector<PluginFeedEntry>& plugins) {
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& p : plugins) {
        nlohmann::json& info =
            subtree["get"]["children"]["plugin"]["children"]["info"]["children"][p.id];
        info["action"] = plugin_action("get.plugin.info", p.id);
        info["help"] = state_note(p);

        nlohmann::json& on =
            subtree["set"]["children"]["plugin"]["children"]["on"]["children"][p.id];
        on["action"] = plugin_action("set.plugin.on", p.id);
        on["help"] = p.enabled ? "already on" : "enable this plugin";

        nlohmann::json& off =
            subtree["set"]["children"]["plugin"]["children"]["off"]["children"][p.id];
        off["action"] = plugin_action("set.plugin.off", p.id);
        off["help"] = p.enabled ? "disable this plugin" : "already off";
    }
    return subtree;
}

} // namespace tui
