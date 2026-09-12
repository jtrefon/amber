#ifndef AGENT_PLUGINS_OPENCODE_GO_H
#define AGENT_PLUGINS_OPENCODE_GO_H

// The OpenCode Go provider plugin: an OpenAI-compatible gateway plus an
// allowance fetch for the Go subscription's rolling/weekly/monthly usage
// windows. The usage endpoint is GET /zen/go/v1/usage with Bearer auth using
// the Go API key.

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <optional>
#include <string>
#include <vector>

namespace agent::plugins {

// Parse the Go usage response body into an AllowanceSnapshot. Public so tests
// can call it with a fixture without touching the network.
std::optional<AllowanceSnapshot> parse_opencode_go_usage(const std::string& body);

class OpencodeGoPlugin : public IPlugin {
public:
    std::string id() const override { return "opencode_go"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "OpenCode Go provider"; }
    std::string description() const override {
        return "OpenCode Go gateway, with subscription usage windows.";
    }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_OPENCODE_GO_H
