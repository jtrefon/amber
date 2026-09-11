#include "deepseek_plugin.h"

#include "agent/extensions.h"
#include "agent/http_get.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace agent::plugins {

namespace {

constexpr const char* kApiBase = "https://api.deepseek.com";
constexpr const char* kDefaultModel = "deepseek-chat";
constexpr const char* kBalancePath = "/user/balance";

} // namespace

double parse_deepseek_balance(const std::string& body) {
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return -1.0;
    if (!j.contains("balance_infos") || !j["balance_infos"].is_array())
        return -1.0;
    // Sum across all currency entries; the first is usually the only one.
    double total = 0.0;
    bool found = false;
    for (const auto& info : j["balance_infos"]) {
        if (!info.is_object())
            continue;
        if (info.contains("total_balance") && info["total_balance"].is_string()) {
            try {
                total += std::stod(info["total_balance"].get<std::string>());
                found = true;
            } catch (...) {
            }
        } else if (info.contains("total_balance") && info["total_balance"].is_number()) {
            total += info["total_balance"].get<double>();
            found = true;
        }
    }
    return found ? total : -1.0;
}

std::vector<std::unique_ptr<Capability>> DeepseekPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "deepseek";
    preset.api_base = kApiBase;
    preset.default_model = kDefaultModel;
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));

    caps.push_back(std::make_unique<WalletCapability>(
        [](const Config& cfg) -> std::optional<double> {
            if (cfg.api_key.empty())
                return std::nullopt;
            const std::optional<std::string> body =
                http_get_with_bearer(std::string(kApiBase) + kBalancePath, cfg.api_key);
            if (!body)
                return std::nullopt;
            const double balance = parse_deepseek_balance(*body);
            if (balance < 0.0)
                return std::nullopt;
            return balance;
        }));
    return caps;
}

} // namespace agent::plugins
