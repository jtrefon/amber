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
// into an WalletWindow with a derived percent_used.
WalletWindow window_from_cc_json(const std::string& label, const nlohmann::json& j) {
    WalletWindow w;
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

double number_or(const nlohmann::json& j, const char* key, double fallback) {
    if (j.contains(key) && j[key].is_number())
        return j[key].get<double>();
    return fallback;
}

// Monthly credits: the remaining balance, not a rolling window.
void apply_credits(const nlohmann::json& credits, WalletSnapshot& snap) {
    const double total = number_or(credits, "monthlyCredits", 0) +
                         number_or(credits, "purchasedCredits", 0) +
                         number_or(credits, "freeCredits", 0);
    if (total <= 0)
        return;
    snap.credits_balance = total;
    WalletWindow monthly;
    monthly.label = "monthly";
    monthly.remaining = total;
    monthly.entitlement = total;
    snap.windows.push_back(monthly);
}

// windowLimits sits at the top level, or nested under credits.
const nlohmann::json* find_window_limits(const nlohmann::json& j) {
    if (j.contains("windowLimits") && j["windowLimits"].is_object())
        return &j["windowLimits"];
    if (j.contains("credits") && j["credits"].is_object() &&
        j["credits"].contains("windowLimits") && j["credits"]["windowLimits"].is_object())
        return &j["credits"]["windowLimits"];
    return nullptr;
}

// Rolling windows: 5-hour and weekly, with used/cap/resetAt.
void apply_windows(const nlohmann::json& wl, WalletSnapshot& snap) {
    if (wl.contains("fiveHour") && wl["fiveHour"].is_object())
        snap.windows.push_back(window_from_cc_json("5h", wl["fiveHour"]));
    if (wl.contains("weekly") && wl["weekly"].is_object())
        snap.windows.push_back(window_from_cc_json("7d", wl["weekly"]));
}

} // namespace

std::optional<WalletSnapshot> parse_commandcode_credits(const std::string& body) {
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    WalletSnapshot snap;
    snap.unit = "USD";
    snap.currency = "USD";

    if (j.contains("credits") && j["credits"].is_object())
        apply_credits(j["credits"], snap);

    if (const nlohmann::json* wl = find_window_limits(j))
        apply_windows(*wl, snap);

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

    caps.push_back(
        std::make_unique<WalletCapability>([](const Config& cfg) -> std::optional<WalletSnapshot> {
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
