#include "kilocode_plugin.h"

#include "agent/extensions.h"

#include <curl/curl.h>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <thread>

namespace agent::plugins {

struct BalanceState {
    std::atomic<double> balance{-1.0};
    std::atomic<bool> valid{false}; // a fetch has completed
    std::atomic<bool> inflight{false};
    std::chrono::steady_clock::time_point next_poll{};
};

namespace {

constexpr const char* kBalanceUrl = "https://api.kilo.ai/api/profile/balance";
constexpr const char* kGatewayBase = "https://api.kilo.ai/api/gateway";
constexpr const char* kDefaultModel = "kilo-auto/free";

size_t write_body(char* ptr, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

double fetch_kilocode_balance(const std::string& token, const std::string& api_base) {
    if (token.empty())
        return -1.0;
    std::string url = api_base.empty() ? kBalanceUrl : api_base + "/profile/balance";
    auto curl = curl_easy_init();
    if (!curl)
        return -1.0;

    std::string body;
    std::string auth = "Authorization: Bearer " + token;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status < 200 || status >= 300)
        return -1.0;

    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
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

bool KilocodePlugin::initialize(const PluginContext& ctx) {
    ctx_ = &ctx;
    state_ = std::make_shared<BalanceState>();
    return true;
}

void KilocodePlugin::shutdown() {
    // The fetch thread holds its own shared_ptr to the state, so dropping ours
    // here cannot race an in-flight request.
    state_.reset();
}

void KilocodePlugin::tick() {
    const Config* cfg = config();
    if (!state_ || !cfg)
        return;
    const std::string token = balance_token();
    if (token.empty())
        return;

    auto state = state_;
    const auto now = std::chrono::steady_clock::now();
    if (now < state->next_poll)
        return;
    state->next_poll = now + std::chrono::seconds(60);
    if (state->inflight.exchange(true))
        return;

    // Throttled and off-thread: a slow endpoint must never block a paint, and
    // the state outlives the plugin if a fetch is still running at shutdown.
    std::thread([state, token, base = cfg->api_base] {
        const double balance = fetch_kilocode_balance(token, base);
        state->balance.store(balance);
        state->valid.store(true);
        state->inflight.store(false);
    }).detach();
}

std::string KilocodePlugin::balance_token() const {
    const Config* cfg = config();
    if (!cfg)
        return {};
    const std::string override_token = cfg->kilo_balance_token;
    if (!override_token.empty())
        return override_token;
    // The gateway key doubles as the account token, so it is only usable when
    // this provider is the active one.
    if (cfg->provider_name != "kilocode")
        return {};
    return cfg->api_key;
}

std::string KilocodePlugin::balance_label() const {
    if (!state_ || balance_token().empty() || !state_->valid.load())
        return {};
    const double balance = state_->balance.load();
    if (balance < 0)
        return "kilo balance \u2014"; // fetch failed / offline
    char buf[48];
    std::snprintf(buf, sizeof(buf), "kilo $%.2f", balance);
    return buf;
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

    // The balance readout: a status segment like any other, so the bar has no
    // provider-specific code in it.
    caps.push_back(std::make_unique<StatusSegmentCapability>(
        "kilo_balance", 800, 5, [this](const StatusSnapshot&) {
            const std::string label = balance_label();
            if (label.empty())
                return StatusText{};
            return StatusText{"  " + label, StatusTone::Dim};
        }));
    return caps;
}

} // namespace agent::plugins
