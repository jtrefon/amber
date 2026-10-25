#ifndef AGENT_SKILL_INDEX_H
#define AGENT_SKILL_INDEX_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "agent/http_get.h"
#include "agent/process.h"

namespace agent {

// One row from a registry. `id` is the stable "{source}/{slug}" key; `name` is
// what the user reads and what a search ranks. Both are sanitized before they
// leave the adapter.
struct SkillRef {
    std::string id;
    std::string name;
    std::string source; // "owner/repo" or a host
    int installs = 0;
    bool duplicate = false;
};

struct SkillPage {
    std::vector<SkillRef> skills;
    int total = 0;
    bool cached = false; // served from the on-disk cache
    bool stale = false;  // cache older than the TTL, served because live failed
};

enum class IndexError : unsigned char {
    None,
    Disabled, // skills_index = none
    Offline,  // no transport at all
    Timeout,
    Cancelled,
    RateLimited,
    Http,      // a non-2xx status other than 429
    Malformed, // the body was not the documented shape
};

struct IndexFailure {
    IndexError kind = IndexError::None;
    long status = 0;
    int retry_after_s = 0;
    std::string detail; // fixed text, never remote content
};

struct IndexResult {
    std::optional<SkillPage> page;
    IndexFailure error;

    bool ok() const noexcept { return page.has_value(); }
};

struct SourceStatus {
    std::string name;
    bool enabled = false;
    bool authenticated = false;
    long long fetched_at_ms = 0; // last successful live fetch, 0 = never
};

struct SkillIndexOptions {
    // Empty disables the adapter entirely (skills_index = none).
    std::string base_url = "https://skills.sh";
    // Empty means $XDG_CACHE_HOME/amber/skill-index.
    std::string cache_root;
    // Optional bearer for the v1 API; the unauthenticated search ignores it.
    std::string token;
    long timeout_s = 15;
    long long cache_ttl_ms = 60LL * 1000;
    // Injected so a test can run without curl and without a clock.
    HttpGet http;
    std::function<long long()> now_ms;
};

// The registry capability. Discovery is the only operation in this slice;
// detail/audit enrichment arrives with the GitHub adapter, and the interface
// grows then rather than pretending to support what it cannot answer.
//
// THREAD SAFETY: one instance belongs to one worker thread. Public methods are
// const but may write the cache and the last-fetch stamp; callers do not share
// an instance across threads.
class SkillIndex {
public:
    virtual ~SkillIndex() = default;

    virtual IndexResult search(const std::string& query, int limit,
                               const CancellationToken& cancel) const = 0;

    virtual std::vector<SourceStatus> sources() const = 0;
};

// The cache root the default options use.
std::string skills_index_cache_dir();

// "https://<base>/api/search?q=<encoded>&limit=<n>", base's trailing slash
// tolerated. Exposed so the encoding and clamping are unit-tested directly.
std::string skills_search_url(const std::string& base_url, const std::string& query, int limit);

// Parse the documented `{skills:[{id,source,name,installs}],count}` body.
// Returns `Malformed` for a shape that is not that; individual entries with a
// missing required field are skipped (forward compatibility), and every string
// is sanitized before it is stored.
IndexResult parse_skills_search(const std::string& body, int limit, bool cached);

// The unauthenticated adapter over skills.sh's `/api/search` (the endpoint the
// official CLI uses). Reads and writes a short-lived on-disk cache and falls
// back to a stale page when the live request fails, labelling it `stale`.
class SkillsShIndex final : public SkillIndex {
public:
    explicit SkillsShIndex(SkillIndexOptions opts);

    IndexResult search(const std::string& query, int limit,
                       const CancellationToken& cancel) const override;
    std::vector<SourceStatus> sources() const override;

private:
    // One live request: build the URL, call the injected HTTP function, map the
    // answer, write the cache. Cache fallback belongs to search(), which is the
    // only caller that knows whether a page was already on disk.
    IndexResult fetch(const std::string& query, int limit, long long now,
                      const CancellationToken& cancel) const;

    SkillIndexOptions opts_;
    mutable long long last_fetch_ms_ = 0;
};

} // namespace agent

#endif // AGENT_SKILL_INDEX_H
