#ifndef AGENT_PLUGINS_KILOCODE_H
#define AGENT_PLUGINS_KILOCODE_H

// The kilocode provider plugin: the OpenAI-compatible gateway plus the two
// things that are genuinely kilo-specific — an account balance readout and the
// convention that the gateway key IS the account token.
//
// The readout used to live in the TUI (a poll loop, an atomic cache, and a
// hardcoded status-bar segment). It is provider behaviour, so it belongs to
// the provider: the plugin polls on tick, caches the value, and renders the
// segment. The UI is left with no idea that kilo.ai exists.

#include "agent/plugin_v2.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

// Fetch the account balance in USD. Returns a negative value on any transport,
// HTTP, or parse failure, so the caller can decide not to show a readout.
double fetch_kilocode_balance(const std::string& token, const std::string& api_base);

class KilocodePlugin : public IPlugin {
public:
    std::string id() const override { return "kilocode"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Kilocode provider"; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;
    void tick() override;

    std::vector<std::unique_ptr<Capability>> capabilities() override;

    // The token the balance readout uses: the explicit override when set, else
    // this provider's api_key (its gateway key doubles as the account token).
    // Empty when the active provider is not kilocode or no key is configured.
    std::string balance_token() const;

    // Cached balance label for the status bar; "" when there is nothing honest
    // to show yet.
    std::string balance_label() const;

private:
    const Config* config_ = nullptr;
    // Cached fetch result. Shared with the detached fetch thread so an
    // in-flight request at shutdown cannot write freed memory.
    std::shared_ptr<struct BalanceState> state_;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_KILOCODE_H
