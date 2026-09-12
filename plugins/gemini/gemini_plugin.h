#ifndef AGENT_PLUGINS_GEMINI_H
#define AGENT_PLUGINS_GEMINI_H

// The Gemini provider plugin: a genuinely different wire protocol delivered as
// a plugin. It contributes one dialect, one provider preset, and the settings
// that describe the endpoint — nothing else, and nothing in the transport, the
// agent loop, or the UI knows this file exists.

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

class GeminiPlugin : public IPlugin {
public:
    std::string id() const override { return "gemini"; }
    std::string version() const override { return "0.4.0"; }
    std::string name() const override { return "Gemini provider"; }
    std::string description() const override { return "Google Gemini, native protocol."; }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;

    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_GEMINI_H
