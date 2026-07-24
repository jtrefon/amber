// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/model_probe.h"
#include "agent/debug_log.h"

#include <curl/curl.h>
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

} // namespace

ServerInfo parse_models(const std::string& body) {
    ServerInfo info;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return info;

    const json* entry = nullptr;
    if (j.contains("data") && j["data"].is_array() && !j["data"].empty())
        entry = &j["data"][0];
    else if (j.contains("models") && j["models"].is_array() &&
             !j["models"].empty())
        entry = &j["models"][0];
    if (!entry) return info;

    const json& e = *entry;
    if (e.contains("id") && e["id"].is_string())
        info.model = e["id"].get<std::string>();
    else if (e.contains("model") && e["model"].is_string())
        info.model = e["model"].get<std::string>();
    else if (e.contains("name") && e["name"].is_string())
        info.model = e["name"].get<std::string>();

    if (e.contains("meta") && e["meta"].is_object()) {
        const json& m = e["meta"];
        info.context_size = read_int(m, "n_ctx");
        info.context_train = read_int(m, "n_ctx_train");
    }
    if (info.context_size == 0) info.context_size = read_int(e, "n_ctx");
    if (info.context_train == 0) info.context_train = read_int(e, "n_ctx_train");

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
    LLMClient client(cfg);
    ServerInfo info = client.probe_server();
    merge_server_info(cfg, info);
    return info;
}

std::vector<std::string> parse_model_list(const std::string& body) {
    std::vector<std::string> out;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return out;

    const json* arr = nullptr;
    if (j.contains("data") && j["data"].is_array())
        arr = &j["data"];
    else if (j.contains("models") && j["models"].is_array())
        arr = &j["models"];
    if (!arr) return out;

    for (const auto& e : *arr) {
        if (e.contains("id") && e["id"].is_string())
            out.push_back(e["id"].get<std::string>());
        else if (e.contains("model") && e["model"].is_string())
            out.push_back(e["model"].get<std::string>());
        else if (e.contains("name") && e["name"].is_string())
            out.push_back(e["name"].get<std::string>());
    }
    return out;
}

std::vector<std::string> list_models(const Config& cfg) {
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
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(c);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return {};
    return parse_model_list(response);
}

} // namespace agent
