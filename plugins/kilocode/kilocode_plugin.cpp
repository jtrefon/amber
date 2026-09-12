#include "kilocode_plugin.h"

#include "agent/extensions.h"
#include "agent/http_get.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace agent::plugins {

namespace {

constexpr const char* kBalanceUrl = "https://api.kilo.ai/api/profile/balance";
constexpr const char* kGatewayBase = "https://api.kilo.ai/api/gateway";
constexpr const char* kDefaultModel = "kilo-auto/free";

} // namespace

std::string kilocode_balance_url() {
    return kBalanceUrl;
}

std::string kilocode_balance_token(const Config& cfg) {
    if (!cfg.kilo_balance_token.empty())
        return cfg.kilo_balance_token;
    // The gateway key doubles as the account token, so it is only usable when
    // this provider is the active one.
    if (cfg.provider_name != "kilocode")
        return {};
    return cfg.api_key;
}

double fetch_kilocode_balance(const std::string& token) {
    const std::optional<std::string> body = http_get_with_bearer(kilocode_balance_url(), token);
    if (!body)
        return -1.0; // no token, transport failure, or non-2xx

    nlohmann::json j = nlohmann::json::parse(*body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return -1.0;
    // Accept either a bare number or an object carrying one; anything else is
    // "unknown" rather than a wrong number on the bar.
    if (j.contains("balance") && j["balance"].is_number())
        return j["balance"].get<double>();
    if (j.contains("available") && j["available"].is_number())
        return j["available"].get<double>();
    return -1.0;
}

std::vector<std::unique_ptr<Capability>> KilocodePlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;

    ProviderCapability::Preset preset;
    preset.name = "kilocode";
    preset.api_base = kGatewayBase;
    preset.default_model = kDefaultModel;
    preset.requires_key = true;
    // Presets only: the gateway speaks the shared openai protocol, so this
    // plugin must not claim (or take away) that dialect.
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));

    // The wallet: the fetch only. Polling, caching and rendering belong to the
    // runtime, so every provider's readout behaves the same way and this plugin
    // carries no threads, no timers and no bar code.
    caps.push_back(
        std::make_unique<WalletCapability>([](const Config& cfg) -> std::optional<WalletSnapshot> {
            const std::string token = kilocode_balance_token(cfg);
            if (token.empty())
                return std::nullopt;
            const double balance = fetch_kilocode_balance(token);
            if (balance < 0.0)
                return std::nullopt; // failed or unavailable: claim nothing
            return WalletSnapshot::of_balance(balance);
        }));
    return caps;
}

} // namespace agent::plugins
