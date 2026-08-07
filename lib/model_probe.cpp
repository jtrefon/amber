
#include "agent/model_probe.h"
#include "agent/debug_log.h"

#include <curl/curl.h>
#include <set>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

size_t probe_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

auto read_int = [](const json& o, const char* k) -> int {
    auto it = o.find(k);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : 0;
};

// Parse a single model entry into a ModelInfo. The id may come from any of
// the shapes servers use ("id", "model", "name").
ModelInfo parse_entry(const json& e) {
    ModelInfo m;
    if (e.contains("id") && e["id"].is_string())
        m.id = e["id"].get<std::string>();
    else if (e.contains("model") && e["model"].is_string())
        m.id = e["model"].get<std::string>();
    else if (e.contains("name") && e["name"].is_string())
        m.id = e["name"].get<std::string>();
    if (e.contains("meta") && e["meta"].is_object()) {
        const json& meta = e["meta"];
        m.context = read_int(meta, "n_ctx");
        m.context_train = read_int(meta, "n_ctx_train");
    }
    if (m.context == 0) m.context = read_int(e, "n_ctx");
    if (m.context_train == 0) m.context_train = read_int(e, "n_ctx_train");
    return m;
}

// The model array from a /v1/models body ("data" or "models" key).
const json* model_array(const json& j) {
    if (j.contains("data") && j["data"].is_array())
        return &j["data"];
    if (j.contains("models") && j["models"].is_array())
        return &j["models"];
    return nullptr;
}

// GET the /v1/models body into `response`. Returns CURLE_OK on success.
CURLcode fetch_models(const Config& cfg, std::string& response) {
    CURL* c = curl_easy_init();
    if (!c) return CURLE_FAILED_INIT;

    struct curl_slist* headers = nullptr;
    if (!cfg.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, cfg.models_url().c_str());
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

ServerInfo parse_models(const std::string& body,
                        const std::string& preferred_model) {
    ServerInfo info;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return info;

    const json* arr = model_array(j);
    if (!arr || arr->empty()) return info;

    // The active model's entry wins (a router may list models without
    // context metadata ahead of the one in use); otherwise the first entry
    // that reports a positive window; otherwise the first entry.
    const json* chosen = nullptr;
    if (!preferred_model.empty()) {
        for (const auto& e : *arr) {
            if (!e.is_object()) continue;
            for (const char* k : {"id", "model", "name"}) {
                if (e.contains(k) && e[k].is_string() &&
                    e[k].get<std::string>() == preferred_model) {
                    chosen = &e;
                    break;
                }
            }
            if (chosen) break;
        }
    }
    if (!chosen) {
        for (const auto& e : *arr)
            if (e.is_object() && parse_entry(e).context > 0) {
                chosen = &e;
                break;
            }
    }
    if (!chosen) chosen = &(*arr)[0];

    ModelInfo m = parse_entry(*chosen);
    info.model = m.id;
    info.context_size = m.context;
    info.context_train = m.context_train;
    info.ok = !info.model.empty() || info.context_size > 0;
    return info;
}

ServerInfo probe_server(const Config& cfg) {
    std::string response;
    CURL* c = curl_easy_init();
    if (!c) return {};

    struct curl_slist* headers = nullptr;
    if (!cfg.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, cfg.models_url().c_str());
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode rc = curl_easy_perform(c);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg.debug_log, "probe-error",
                  std::string(curl_easy_strerror(rc)));
        return {};
    }
    debug_log(cfg.debug_log, "probe", response);
    return parse_models(response);
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

std::vector<ModelInfo> parse_model_list_info(const std::string& body) {
    std::vector<ModelInfo> out;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return out;

    const json* arr = model_array(j);
    if (!arr) return out;

    // Servers sometimes list the same model multiple times (aliases, quant
    // variants with the same id); the UI and model-set validation expect a
    // unique list.
    std::set<std::string> seen;
    for (const auto& e : *arr) {
        ModelInfo m = parse_entry(e);
        if (!m.id.empty() && seen.insert(m.id).second)
            out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::string> parse_model_list(const std::string& body) {
    std::vector<std::string> out;
    for (const auto& m : parse_model_list_info(body))
        out.push_back(m.id);
    return out;
}

std::vector<ModelInfo> list_model_info(const Config& cfg) {
    std::string response;
    if (fetch_models(cfg, response) != CURLE_OK) return {};
    return parse_model_list_info(response);
}

std::vector<std::string> list_models(const Config& cfg) {
    std::vector<std::string> out;
    for (const auto& m : list_model_info(cfg))
        out.push_back(m.id);
    return out;
}

} // namespace agent
