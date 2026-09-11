#ifndef AGENT_PLUGINS_KILOCODE_H
#define AGENT_PLUGINS_KILOCODE_H

// The kilocode provider plugin: the OpenAI-compatible gateway plus the two
// things that are genuinely kilo-specific — an account balance readout and the
// convention that the gateway key IS the account token.
//
// The plugin supplies only the *fetch*: the runtime owns when to poll, how to
// cache, and how the bar renders it (see WalletRegistry). It used to carry its
// own poll loop, atomic cache and status segment, which is exactly the
// duplication the framework now removes.

#include "agent/plugin_v2.h"

#include <string>
#include <vector>

namespace agent::plugins {

// The balance endpoint. This is a fixed kilo.ai API, NOT the gateway base the
// provider chats through: the two live under different paths
// (api.kilo.ai/api/profile/balance vs api.kilo.ai/api/gateway). Deriving the
// balance URL from `api_base` produced ".../api/gateway/profile/balance",
// which 404s — the readout silently stopped working.
std::string kilocode_balance_url();

// The token the balance readout uses: the explicit override when set, else the
// gateway key when this provider is the active one (its key doubles as the
// account token). Empty when there is nothing usable.
std::string kilocode_balance_token(const Config& cfg);

// Fetch the account balance in USD. Returns a negative value on any transport,
// HTTP, or parse failure, so the caller can decide not to show a readout.
double fetch_kilocode_balance(const std::string& token);

class KilocodePlugin : public IPlugin {
public:
    std::string id() const override { return "kilocode"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Kilocode provider"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_KILOCODE_H
