#ifndef AGENT_PLUGINS_OPENROUTER_H
#define AGENT_PLUGINS_OPENROUTER_H

// The OpenRouter provider plugin: an OpenAI-compatible router, so it
// contributes presets and speaks the shared openai dialect — no wire protocol
// of its own, and nothing provider-specific in the core.
//
// It also declares a wallet. OpenRouter reports a *per-key* spend cap
// (`GET /v1/key` → `limit_remaining`), which is what a normal inference key
// can see; account-wide credits need a management key and are deliberately out
// of scope.

#include "agent/plugin_v2.h"

#include <optional>
#include <string>
#include <vector>

namespace agent::plugins {

// The key-details endpoint on the configured base.
std::string openrouter_key_url(const std::string& api_base);

// Parse `GET /v1/key`: the remaining allowance for this key. Returns nullopt
// when the key is uncapped (the API reports no limit) or the body is not the
// expected shape — an uncapped key has no "remaining" to show, and inventing
// one would be worse than showing nothing.
std::optional<double> parse_openrouter_key(const std::string& body);

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
