#ifndef AGENT_PLUGINS_HELLO_PLUGIN_H
#define AGENT_PLUGINS_HELLO_PLUGIN_H

// The command quickstart.
//
// Registers the /hello slash-command namespace, whose `greet` leaf returns a
// string. It demonstrates the whole path a command plugin takes: build the
// logic, expose it as a callback, declare the JSON mapping, and let the host
// bind it into the slash engine. See docs/spec/plugins/developer-guide.md.

#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

class HelloPlugin : public IPlugin {
public:
    std::string id() const override { return "hello"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Hello"; }
    std::string description() const override {
        return "Example slash command: /hello greet <name>.";
    }
    std::string category() const override { return plugin_category::kUi; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_HELLO_PLUGIN_H
