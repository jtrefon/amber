#include "commandcode_plugin.h"

#include "agent/extensions.h"
#include "agent/http_get.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace agent::plugins {

namespace {

constexpr const char* kApiBase = "https://api.commandcode.ai/provider/v1";
constexpr const char* kBillingBase = "https://api.commandcode.ai";
constexpr const char* kDefaultModel = "gpt-4.1";
constexpr const char* kCreditsPath = "/alpha/billing/credits";

// Map a CommandCode windowLimits entry (used/cap in USD, resetAt in epoch ms)
// into an AllowanceWindow with a derived percent_used.
AllowanceWindow window_from_cc_json(const std::string& label, const nlohmann::json& j) {
    AllowanceWindow w;
    w.label = label;
    double used = -1, cap = -1;
    if (j.contains("used") && j["used"].is_number())
        used = j["used"].get<double>();
    if (j.contains("cap") && j["cap"].is_number())
        cap = j["cap"].get<double>();
    if (used >= 0 && cap > 0)
        w.percent_used = (used / cap) * 100.0;
    if (used >= 0)
        w.remaining = cap - used;
    if (cap >= 0)
        w.entitlement = cap;
    if (j.contains("resetAt") && j["resetAt"].is_number()) {
        // epoch ms → ISO string is non-trivial; store the raw value as a
        // string so /get can display it and tests can verify it.
        w.resets_at = std::to_string(j["resetAt"].get<long long>());
    }
    return w;
}

} // namespace

std::optional<AllowanceSnapshot> parse_commandcode_credits(const std::string& body) {
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    AllowanceSnapshot snap;
    snap.unit = "USD";
    snap.currency = "USD";

    // Monthly credits: the remaining balance, not a rolling window.
    if (j.contains("credits") && j["credits"].is_object()) {
        const auto& credits = j["credits"];
        double monthly = 0, purchased = 0, free_c = 0;
        if (credits.contains("monthlyCredits") && credits["monthlyCredits"].is_number())
            monthly = credits["monthlyCredits"].get<double>();
        if (credits.contains("purchasedCredits") && credits["purchasedCredits"].is_number())
            purchased = credits["purchasedCredits"].get<double>();
        if (credits.contains("freeCredits") && credits["freeCredits"].is_number())
            free_c = credits["freeCredits"].get<double>();
        const double total = monthly + purchased + free_c;
        if (total > 0) {
            snap.credits_balance = total;
            AllowanceWindow monthly_w;
            monthly_w.label = "monthly";
            monthly_w.remaining = total;
            monthly_w.entitlement = total;
            snap.windows.push_back(monthly_w);
        }
    }

    // Rolling windows: 5-hour and weekly, with used/cap/resetAt.
    const nlohmann::json* wl = nullptr;
    if (j.contains("windowLimits") && j["windowLimits"].is_object())
        wl = &j["windowLimits"];
    else if (j.contains("credits") && j["credits"].is_object() &&
             j["credits"].contains("windowLimits") && j["credits"]["windowLimits"].is_object())
        wl = &j["credits"]["windowLimits"];
    if (wl) {
        if (wl->contains("fiveHour") && (*wl)["fiveHour"].is_object())
            snap.windows.push_back(window_from_cc_json("5h", (*wl)["fiveHour"]));
        if (wl->contains("weekly") && (*wl)["weekly"].is_object())
            snap.windows.push_back(window_from_cc_json("7d", (*wl)["weekly"]));
    }

    if (snap.windows.empty() && !snap.credits_balance)
        return std::nullopt;
    return snap;
}

std::vector<std::unique_ptr<Capability>> CommandcodePlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "commandcode";
    preset.api_base = kApiBase;
    preset.default_model = kDefaultModel;
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));

    caps.push_back(std::make_unique<AllowanceCapability>(
        [](const Config& cfg) -> std::optional<AllowanceSnapshot> {
            if (cfg.api_key.empty())
                return std::nullopt;
            const std::optional<std::string> body =
                http_get_with_bearer(std::string(kBillingBase) + kCreditsPath, cfg.api_key);
            if (!body)
                return std::nullopt;
            return parse_commandcode_credits(*body);
        }));
    return caps;
}

} // namespace agent::plugins
