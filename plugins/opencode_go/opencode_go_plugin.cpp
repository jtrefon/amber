#include "opencode_go_plugin.h"

#include "agent/extensions.h"
#include "agent/http_get.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace agent::plugins {

namespace {

constexpr const char* kApiBase = "https://opencode.ai/zen/go/v1";
constexpr const char* kDefaultModel = "glm-5.2";
constexpr const char* kUsagePath = "/usage";

// Map a Go usage window JSON object into an WalletWindow. The Go API
// reports `percent` as used percentage and `resetsAt` as an ISO timestamp.
WalletWindow window_from_json(const std::string& label, const nlohmann::json& j) {
    WalletWindow w;
    w.label = label;
    if (j.contains("percent") && j["percent"].is_number())
        w.percent_used = j["percent"].get<double>();
    if (j.contains("resetsAt") && j["resetsAt"].is_string())
        w.resets_at = j["resetsAt"].get<std::string>();
    return w;
}

} // namespace

std::optional<WalletSnapshot> parse_opencode_go_usage(const std::string& body) {
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    if (!j.contains("usage") || !j["usage"].is_object())
        return std::nullopt;
    const auto& usage = j["usage"];
    WalletSnapshot snap;
    snap.unit = "percent";
    if (usage.contains("rolling") && usage["rolling"].is_object())
        snap.windows.push_back(window_from_json("5h", usage["rolling"]));
    if (usage.contains("weekly") && usage["weekly"].is_object())
        snap.windows.push_back(window_from_json("7d", usage["weekly"]));
    if (usage.contains("monthly") && usage["monthly"].is_object())
        snap.windows.push_back(window_from_json("monthly", usage["monthly"]));
    if (snap.windows.empty())
        return std::nullopt;
    return snap;
}

std::vector<std::unique_ptr<Capability>> OpencodeGoPlugin::capabilities() {
    ProviderCapability::Preset preset;
    preset.name = "opencode_go";
    preset.api_base = kApiBase;
    preset.default_model = kDefaultModel;
    preset.requires_key = true;

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(
        std::make_unique<ProviderCapability>("openai", std::function<std::unique_ptr<Dialect>()>{},
                                             std::vector<ProviderCapability::Preset>{preset}));

    caps.push_back(std::make_unique<WalletCapability>(
        [](const Config& cfg) -> std::optional<WalletSnapshot> {
            if (cfg.api_key.empty())
                return std::nullopt;
            const std::optional<std::string> body =
                http_get_with_bearer(std::string(kApiBase) + kUsagePath, cfg.api_key);
            if (!body)
                return std::nullopt;
            return parse_opencode_go_usage(*body);
        }));
    return caps;
}

} // namespace agent::plugins
