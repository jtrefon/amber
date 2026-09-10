#ifndef AGENT_PLUGINS_CUSTOM_H
#define AGENT_PLUGINS_CUSTOM_H

// The "custom" provider plugin: the user's own endpoint.
//
// It contributes a preset with no endpoint, which is what makes an
// unconfigured custom provider a loud, actionable state ("no endpoint
// configured") rather than a missing entry. The endpoint itself lives in
// ~/.config/amber/providers/custom.conf, which overlays this preset.
//
// With this plugin the core declares no providers at all: every provider in
// amber comes from a plugin, and the core keeps only the file layer that
// reads what the user wrote.

#include "agent/plugin_v2.h"

#include <string>
#include <vector>

namespace agent::plugins {

class CustomPlugin : public IPlugin {
public:
    std::string id() const override { return "custom"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Custom endpoint provider"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CUSTOM_H
