#ifndef AGENT_PLUGINS_SEARCH_GREP_PLUGIN_H
#define AGENT_PLUGINS_SEARCH_GREP_PLUGIN_H

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// The backend capability this plugin declares. Exposed as a free function so a
// host with no plugin runtime (register_default_tools) can install exactly the
// same definition: one definition, two install paths.
std::vector<std::unique_ptr<Capability>> make_grep_backend_capabilities();

class SearchGrepPlugin : public IPlugin {
public:
    std::string id() const override { return "search_grep"; }
    std::string version() const override { return "0.4.0"; }
    std::string name() const override { return "Grep search backend"; }
    std::string description() const override {
        return "The regex backend (grep -rnIE) behind the search tool.";
    }
    std::string category() const override { return plugin_category::kSearch; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        return make_grep_backend_capabilities();
    }
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_SEARCH_GREP_PLUGIN_H
