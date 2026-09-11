#ifndef AGENT_PLUGINS_OPENCODE_ZEN_H
#define AGENT_PLUGINS_OPENCODE_ZEN_H

// The OpenCode Zen provider plugin: an OpenAI-compatible gateway for the
// Chat Completions model families. Zen also serves GPT (Responses API),
// Claude (Anthropic Messages), and Gemini (Google-native) through other
// protocols, but those require a routing dialect not yet implemented — so
// this plugin advertises only the models the shared openai dialect can call.

#include "agent/plugin_v2.h"

#include <string>
#include <vector>

namespace agent::plugins {

class OpencodeZenPlugin : public IPlugin {
public:
    std::string id() const override { return "opencode_zen"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "OpenCode Zen provider"; }
    std::string description() const override {
        return "OpenCode Zen gateway (OpenAI-compatible models).";
    }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_OPENCODE_ZEN_H
