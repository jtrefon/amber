#include "agent/model_probe.h"
#include "agent/debug_log.h"
#include "agent/dialect.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace agent {

namespace fs = std::filesystem;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

namespace {

// One curl GET {api_base}/models -> body. Returns CURLE_OK on HTTP 2xx.
CURLcode fetch_models(const Config& cfg, const Dialect& dialect, std::string& body) {
    const std::string url = dialect.models_url(cfg);
    if (url.empty())
        return CURLE_URL_MALFORMAT;

    CURL* c = curl_easy_init();
    if (!c)
        return CURLE_FAILED_INIT;

    struct curl_slist* headers = nullptr;
    for (const std::string& h : dialect.auth_headers(cfg))
        headers = curl_slist_append(headers, h.c_str());
    if (headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (code < 200 || code >= 300)
            rc = CURLE_HTTP_RETURNED_ERROR;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return rc;
}

// ---------------------------------------------------------------------------
// Catalog cache: ~/.config/amber/cache/models-<hash(api_base+flavor)>.json
// ---------------------------------------------------------------------------

constexpr long long kCatalogTtlMs = 24LL * 3600 * 1000;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string catalog_cache_path(const Config& cfg) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    const std::string key = cfg.api_base + "\n" + cfg.flavor;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "models-%016llx.json", static_cast<unsigned long long>(h));
    return global_config_dir() + "/cache/" + name;
}

// Single-flight slots, keyed by endpoint identity. Concurrent fetchers for
// one endpoint share the in-flight request; a slot lives until the map does
// (removed on completion would re-race with waiters, so slots persist).
struct FetchSlot {
    std::mutex mtx;
    std::condition_variable cv;
    bool active = false;
};

std::mutex& slots_mtx() {
    static std::mutex m;
    return m;
}

std::map<std::string, std::shared_ptr<FetchSlot>>& slots() {
    static std::map<std::string, std::shared_ptr<FetchSlot>> m;
    return m;
}

} // namespace

std::optional<ModelCatalogEntry> model_catalog_read(const Config& cfg) {
    std::ifstream f(catalog_cache_path(cfg));
    if (!f)
        return std::nullopt;
    json j;
    try {
        f >> j;
    } catch (...) {
        return std::nullopt;
    }
    ModelCatalogEntry e;
    try {
        e.body = j.at("body").get<std::string>();
        e.fetched_ms = j.at("fetched_ms").get<long long>();
    } catch (...) {
        return std::nullopt;
    }
    if (e.body.empty())
        return std::nullopt;
    return e;
}

void model_catalog_write(const Config& cfg, const std::string& body) {
    const std::string path = catalog_cache_path(cfg);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f)
            return;
        f << json{{"fetched_ms", now_ms()}, {"body", body}}.dump();
        if (!f)
            return;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
}

bool model_catalog_fresh(const ModelCatalogEntry& entry) {
    return now_ms() - entry.fetched_ms < kCatalogTtlMs;
}

std::optional<CatalogFetchResult> model_catalog_fetch(const Config& cfg, bool force) {
    auto cached = model_catalog_read(cfg);
    if (!force && cached)
        return CatalogFetchResult{*cached, false}; // SWR: serve, caller revalidates async

    const std::string key = cfg.api_base + "\n" + cfg.flavor;
    std::shared_ptr<FetchSlot> slot;
    {
        std::scoped_lock lk(slots_mtx());
        auto& s = slots()[key];
        if (!s)
            s = std::make_shared<FetchSlot>();
        slot = s;
    }

    std::unique_lock lk(slot->mtx);
    if (slot->active) {
        // Join the in-flight request instead of issuing a duplicate.
        slot->cv.wait(lk, [&] { return !slot->active; });
        lk.unlock();
        if (auto e = model_catalog_read(cfg))
            return CatalogFetchResult{*e, true};
        return cached ? std::optional<CatalogFetchResult>{{*cached, false}} : std::nullopt;
    }
    slot->active = true;
    lk.unlock();

    auto dialect = make_dialect(cfg.flavor);
    std::string body;
    const bool fetched = fetch_models(cfg, *dialect, body) == CURLE_OK;
    if (fetched) {
        debug_log(cfg.debug_log, "probe", body);
        model_catalog_write(cfg, body);
    } else {
        debug_log(cfg.debug_log, "probe-error", "fetch failed");
    }

    {
        std::scoped_lock l2(slot->mtx);
        slot->active = false;
    }
    slot->cv.notify_all();

    if (auto e = model_catalog_read(cfg))
        return CatalogFetchResult{*e, fetched};
    return cached ? std::optional<CatalogFetchResult>{{*cached, false}} : std::nullopt;
}

void model_catalog_refresh_async(const Config& cfg, std::function<void(std::function<void()>)> post,
                                 std::function<void(bool)> done) {
    Config copy = cfg;
    std::thread([copy = std::move(copy), post = std::move(post), done = std::move(done)]() mutable {
        auto r = model_catalog_fetch(copy, /*force=*/true);
        post([done = std::move(done), fetched = r && r->fetched] { done(fetched); });
    }).detach();
}

ServerInfo probe_server_cached(const Config& cfg) {
    auto e = model_catalog_read(cfg);
    if (!e)
        return {};
    auto dialect = make_dialect(cfg.flavor);
    // Same preferred-model rule as probe_server: the explicit model's entry
    // wins over the listing's own order.
    return dialect->parse_models_response(e->body, cfg.model_explicit ? cfg.model : "");
}

std::vector<ModelInfo> list_model_info_cached(const Config& cfg) {
    auto e = model_catalog_read(cfg);
    if (!e)
        return {};
    return make_dialect(cfg.flavor)->parse_model_list_response(e->body);
}

void merge_server_info(Config& cfg, const ServerInfo& info) {
    if (!info.ok)
        return;
    if (!cfg.model_explicit && !info.model.empty())
        cfg.model = info.model;
    if (!cfg.context_explicit && info.context_size > 0)
        cfg.context_size = info.context_size;
}

ServerInfo apply_cached_server_autodetect(Config& cfg) {
    ServerInfo info = probe_server_cached(cfg);
    merge_server_info(cfg, info);
    return info;
}

ServerInfo probe_server(const Config& cfg) {
    auto dialect = make_dialect(cfg.flavor);
    return probe_server(cfg, *dialect);
}

ServerInfo probe_server(const Config& cfg, const Dialect& dialect) {
    ServerInfo out;
    auto r = model_catalog_fetch(cfg, /*force=*/true);
    // Prefer the ACTIVE model's entry when the user picked one explicitly: a
    // router (kilocode et al.) lists models in its own order, and the first
    // entry with a context window is not necessarily the one in use — adopting
    // it sizes the gauge and the compression budget to the wrong model.
    if (r)
        out = dialect.parse_models_response(r->entry.body, cfg.model_explicit ? cfg.model : "");
    return out;
}

std::vector<ModelInfo> list_model_info(const Config& cfg) {
    auto dialect = make_dialect(cfg.flavor);
    return list_model_info(cfg, *dialect);
}

std::vector<ModelInfo> list_model_info(const Config& cfg, const Dialect& dialect) {
    auto r = model_catalog_fetch(cfg, /*force=*/true);
    return r ? dialect.parse_model_list_response(r->entry.body) : std::vector<ModelInfo>{};
}

std::vector<std::string> list_models(const Config& cfg) {
    std::vector<std::string> ids;
    for (const ModelInfo& mi : list_model_info(cfg))
        if (!mi.id.empty())
            ids.push_back(mi.id);
    return ids;
}

ServerInfo apply_server_autodetect(Config& cfg, bool force) {
    ServerInfo info;
    auto r = model_catalog_fetch(cfg, force);
    if (r) {
        auto dialect = make_dialect(cfg.flavor);
        info = dialect->parse_models_response(r->entry.body, cfg.model_explicit ? cfg.model : "");
    }
    merge_server_info(cfg, info);
    // When the window is still unknown (no catalog entry, no explicit config)
    // it stays 0: the compression gate applies its own fallback budget and
    // the context gauge hides instead of showing a fabricated number. The
    // HTTP 400 error learner (http_transport.cpp) still corrects downward
    // once the server rejects an oversized request.
    return info;
}

} // namespace agent
