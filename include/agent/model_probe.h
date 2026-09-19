#ifndef AGENT_MODEL_PROBE_H
#define AGENT_MODEL_PROBE_H

#include "agent/config.h"
#include "agent/llm.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class Dialect;

// Query the server's model-listing endpoint and report the active model's id
// and context window (n_ctx). Never throws: on any transport/parse failure it
// returns a ServerInfo with ok == false. The endpoint, auth, and parsing are
// the dialect's (resolved from cfg.flavor); the two-argument overload uses a
// caller-held dialect (the client's). Every fetch is write-through: a
// successful probe repopulates the disk catalog cache.
ServerInfo probe_server(const Config& cfg);
ServerInfo probe_server(const Config& cfg, const Dialect& dialect);

// Fetch all model entries with context info from the configured server.
// Returns empty on error (including a protocol without a listing endpoint).
std::vector<ModelInfo> list_model_info(const Config& cfg);
std::vector<ModelInfo> list_model_info(const Config& cfg, const Dialect& dialect);

// Fetch all model IDs from the configured server. Returns empty on error.
std::vector<std::string> list_models(const Config& cfg);

// ---------------------------------------------------------------------------
// Model catalog cache (stale-while-revalidate)
//
// The GET {api_base}/models response is persisted per endpoint
// (api_base + flavor) under the global config dir. Interactive paths read
// the cache only — never the network — and revalidate in the background
// when the entry is older than the TTL (24h). Blocking callers (agent
// workers, headless hosts) go through model_catalog_fetch(), which is
// single-flight: concurrent fetches for one endpoint share a single HTTP
// request.
// ---------------------------------------------------------------------------

struct ModelCatalogEntry {
    std::string body; // raw models-listing response
    long long fetched_ms = 0;
};

// Disk-only read of the cached catalog for cfg's endpoint. Never touches the
// network; returns nullopt when absent or unparsable.
std::optional<ModelCatalogEntry> model_catalog_read(const Config& cfg);

// Persist a fetched body (atomic tmp+rename). Public for tests; production
// writers go through model_catalog_fetch().
void model_catalog_write(const Config& cfg, const std::string& body);

// True while the entry is younger than the catalog TTL.
bool model_catalog_fresh(const ModelCatalogEntry& entry);

struct CatalogFetchResult {
    ModelCatalogEntry entry; // freshest available body (new fetch or stale fallback)
    bool fetched = false;    // the HTTP fetch succeeded in this call
};

// Blocking cache-through fetch — for non-UI threads only. force=false serves
// any cached entry (fresh or stale) and only reaches the network on a cold
// cache; force=true always revalidates. Stale-if-error: a failed refresh
// still returns the previous cached entry. Returns nullopt only when no
// usable body exists at all.
std::optional<CatalogFetchResult> model_catalog_fetch(const Config& cfg, bool force = false);

// Asynchronous revalidation, safe to call from the UI thread: the fetch runs
// on a detached worker (deduped by the single-flight above) and `done` is
// delivered through `post` (the host's UI-thread queue). `fetched` reports
// whether the network fetch succeeded; the freshest body is in the cache.
void model_catalog_refresh_async(const Config& cfg, std::function<void(std::function<void()>)> post,
                                 std::function<void(bool fetched)> done);

// Cache-only variants of the probe/list functions: identical parsing, zero
// network. For the UI thread.
ServerInfo probe_server_cached(const Config& cfg);
std::vector<ModelInfo> list_model_info_cached(const Config& cfg);

// Cache-only autodetect for interactive startup: applies a cached probe
// result to cfg via merge_server_info, never blocks. On a cold cache cfg is
// untouched and the caller schedules model_catalog_refresh_async.
ServerInfo apply_cached_server_autodetect(Config& cfg);

// The kilo.ai balance readout lives with its provider: see
// plugins/kilocode/kilocode_plugin.h. Core probing knows nothing about it.

} // namespace agent

#endif // AGENT_MODEL_PROBE_H
