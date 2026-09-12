#include "openrouter_plugin.h"

#include "agent/extensions.h"
#include "agent/http_get.h"

#include <nlohmann/json.hpp>

namespace agent::plugins {

namespace {

constexpr const char* kApiBase = "https://openrouter.ai/api/v1";

// A number, or nothing: the API uses null for "no limit", and any other shape
// is a body we do not understand.
std::optional<double> number_or_none(const nlohmann::json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}

} // namespace

std::string openrouter_key_url(const std::string& api_base) {
    return (api_base.empty() ? std::string(kApiBase) : api_base) + "/key";
}

std::optional<double> parse_openrouter_key(const std::string& body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return std::nullopt;
    const nlohmann::json data = parsed.value("data", nlohmann::json::object());
    if (!data.is_object())
        return std::nullopt;

    // `limit_remaining` is the direct answer; when a gateway reports only the
    // cap and the spend, the remainder is arithmetic.
    if (const auto remaining = number_or_none(data, "limit_remaining"))
        return remaining;
    const auto limit = number_or_none(data, "limit");
    const auto usage = number_or_none(data, "usage");
    if (limit && usage)
        return *limit - *usage;
    return std::nullopt; // uncapped: nothing honest to display
}

std::vector<std::unique_ptr<Capability>> OpenRouterPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;

    ProviderCapability::Preset preset;
    preset.name = "openrouter";
    preset.api_base = kApiBase;
    preset.default_model = "openai/gpt-4o";
    preset.requires_key = true;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));

    // The wallet: this key's remaining allowance, when the key is capped.
    caps.push_back(
        std::make_unique<WalletCapability>([](const Config& cfg) -> std::optional<WalletSnapshot> {
            if (cfg.provider_name != "openrouter" || cfg.api_key.empty())
                return std::nullopt;
            const std::optional<std::string> body =
                http_get_with_bearer(openrouter_key_url(cfg.api_base), cfg.api_key);
            if (!body)
                return std::nullopt;
            const std::optional<double> amount = parse_openrouter_key(*body);
            if (!amount)
                return std::nullopt;
            return WalletSnapshot::of_balance(*amount);
        }));
    return caps;
}

} // namespace agent::plugins
