#ifndef AGENT_PLUGINS_OPENROUTER_H
#define AGENT_PLUGINS_OPENROUTER_H

// The OpenRouter provider plugin: an OpenAI-compatible router, so it
// contributes presets and speaks the shared openai dialect — no wire protocol
// of its own, and nothing provider-specific in the core.

#include "agent/plugin_v2.h"

#include <string>
#include <vector>

namespace agent::plugins {

class OpenRouterPlugin : public IPlugin {
public:
    std::string id() const override { return "openrouter"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "OpenRouter provider"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_OPENROUTER_H
