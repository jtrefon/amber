#ifndef AGENT_PLUGINS_COMMANDCODE_H
#define AGENT_PLUGINS_COMMANDCODE_H

// The CommandCode provider plugin: an OpenAI-compatible gateway plus an
// allowance fetch for the three-tier usage system (5-hour rolling, weekly
// rolling, monthly credits). The usage data comes from the /alpha/billing
// endpoints, authenticated with the same API key as the Provider API.
//
// The /alpha surface is undocumented and may change; the fetch degrades to
// nullopt on any failure so the bar never shows a wrong number.

#include "agent/extensions.h"
#include "agent/plugin_v2.h"

#include <optional>
#include <string>
#include <vector>

namespace agent::plugins {

// Parse the /alpha/billing/credits response body into an AllowanceSnapshot.
// Public so tests can call it with a fixture without touching the network.
std::optional<AllowanceSnapshot> parse_commandcode_credits(const std::string& body);

class CommandcodePlugin : public IPlugin {
public:
    std::string id() const override { return "commandcode"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "CommandCode provider"; }
    std::string description() const override {
        return "CommandCode gateway, with 5h/weekly/monthly usage windows.";
    }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_COMMANDCODE_H
