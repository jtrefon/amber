#ifndef AGENT_PLUGINS_ANTHROPIC_H
#define AGENT_PLUGINS_ANTHROPIC_H

// The Anthropic provider plugin: it provides the Messages API dialect (the
// one that used to be registered by the core) and its preset.

#include "agent/plugin_v2.h"

#include <string>
#include <vector>

namespace agent::plugins {

class AnthropicPlugin : public IPlugin {
public:
    std::string id() const override { return "anthropic"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Anthropic provider"; }
    std::string description() const override { return "Anthropic Messages API, native protocol."; }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_ANTHROPIC_H
