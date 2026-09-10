
#include "agent/model_probe.h"
#include "agent/debug_log.h"
#include "agent/dialect.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

size_t probe_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// GET the dialect's model-listing endpoint into `response`. Returns a non-OK
// CURLcode on transport failure or when the protocol has no listing endpoint.
CURLcode fetch_models(const Config& cfg, const Dialect& dialect,
                      std::string& response) {
    const std::string url = dialect.models_url(cfg);
    if (url.empty()) return CURLE_URL_MALFORMAT;

    CURL* c = curl_easy_init();
    if (!c) return CURLE_FAILED_INIT;

    struct curl_slist* headers = nullptr;
    for (const auto& h : dialect.auth_headers(cfg))
        headers = curl_slist_append(headers, h.c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(c);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return rc;
}

} // namespace

ServerInfo probe_server(const Config& cfg, const Dialect& dialect) {
    std::string response;
    if (fetch_models(cfg, dialect, response) != CURLE_OK) {
        debug_log(cfg.debug_log, "probe-error", "fetch failed");
        return {};
    }
    debug_log(cfg.debug_log, "probe", response);
    // Prefer the ACTIVE model's entry when the user picked one explicitly: a
    // router (kilocode et al.) lists models in its own order, and the first
    // entry with a context window is not necessarily the one in use — adopting
    // it sizes the gauge and the compression budget to the wrong model.
    return dialect.parse_models_response(
        response, cfg.model_explicit ? cfg.model : "");
}

ServerInfo probe_server(const Config& cfg) {
    auto dialect = make_dialect(cfg.flavor);
    return probe_server(cfg, *dialect);
}

void merge_server_info(Config& cfg, const ServerInfo& info) {
    if (!info.ok) return;
    if (!cfg.model_explicit && !info.model.empty())
        cfg.model = info.model;
    if (!cfg.context_explicit && info.context_size > 0)
        cfg.context_size = info.context_size;
}

ServerInfo apply_server_autodetect(Config& cfg) {
    HttpLLMClient client(cfg);
    ServerInfo info = client.probe_server();
    merge_server_info(cfg, info);
    // When the window is still unknown (no probe result, no explicit config)
    // it stays 0: the compression gate applies its own fallback budget and
    // the context gauge hides instead of showing a fabricated number. The
    // HTTP 400 error learner (http_transport.cpp) still corrects downward
    // once the server rejects an oversized request.
    return info;
}

std::vector<ModelInfo> list_model_info(const Config& cfg,
                                       const Dialect& dialect) {
    std::string response;
    if (fetch_models(cfg, dialect, response) != CURLE_OK) return {};
    return dialect.parse_model_list_response(response);
}

std::vector<ModelInfo> list_model_info(const Config& cfg) {
    auto dialect = make_dialect(cfg.flavor);
    return list_model_info(cfg, *dialect);
}

std::vector<std::string> list_models(const Config& cfg) {
    std::vector<std::string> out;
    for (const auto& m : list_model_info(cfg))
        out.push_back(m.id);
    return out;
}

double fetch_kilo_balance(const std::string& token) {
    if (token.empty()) return -1.0;
    CURL* c = curl_easy_init();
    if (!c) return -1.0;

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
                                ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL,
                     "https://api.kilo.ai/api/profile/balance");
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || http_code < 200 || http_code >= 300) return -1.0;
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.contains("balance") ||
        !j["balance"].is_number())
        return -1.0;
    return j["balance"].get<double>();
}

std::string resolve_kilo_balance_token(const Config& cfg) {
    if (!cfg.kilo_balance_token.empty()) return cfg.kilo_balance_token;
    // Providers whose api_key IS the account token (kilocode's gateway key —
    // its models only work with a valid one, and the TUI key prompt stores it
    // as api_key) power the balance readout without extra configuration. The
    // decision is the provider's declared capability, never its name.
    if (cfg.api_key_is_account_token) return cfg.api_key;
    return "";
}

} // namespace agent
