#ifndef AGENT_PLUGINS_DEEPSEEK_H
#define AGENT_PLUGINS_DEEPSEEK_H

// The DeepSeek provider plugin: an OpenAI-compatible direct API with a wallet
// (prepaid balance) readout. DeepSeek has no coding plan — it is pay-as-you-go
// API access, so the balance endpoint is the operationally relevant metric.
//
// GET https://api.deepseek.com/user/balance with Bearer auth returns
// balance_infos[] with total_balance, granted_balance, topped_up_balance,
// and currency.

#include "agent/plugin_core.h"

#include <string>
#include <vector>

namespace agent::plugins {

// Parse the DeepSeek balance response body. Returns the total balance in the
// account's currency, or -1.0 on any failure. Public for fixture tests.
double parse_deepseek_balance(const std::string& body);

class DeepseekPlugin : public IPlugin {
public:
    std::string id() const override { return "deepseek"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "DeepSeek provider"; }
    std::string description() const override {
        return "DeepSeek direct API, with prepaid balance as the wallet.";
    }
    std::string category() const override { return plugin_category::kProvider; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_DEEPSEEK_H
