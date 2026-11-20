#ifndef AGENT_SKILL_INDEX_H
#define AGENT_SKILL_INDEX_H

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
    bool cached = false;         // served from the on-disk cache
    bool stale = false;          // cache older than the TTL, served because live failed
    long long fetched_at_ms = 0; // when the cached page was fetched; 0 for a live page
};

enum class IndexError : unsigned char {
    None,
    Disabled, // skills_index = none
    Offline,  // no transport at all
    Timeout,
    Cancelled,
    RateLimited,
    Http,        // a non-2xx status other than 429
    Malformed,   // the body was not the documented shape
    Unsupported, // this adapter cannot answer that operation
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

// One file inside a skill directory. The git blob SHA is what the ledger
// records and what an update compares, so a rename with identical bytes is not
// mistaken for a content change.
struct SkillAsset {
    std::string path; // relative to the skill directory
    size_t bytes = 0;
    std::string sha;
};

// Everything a detail pane and an installer need, from one repo listing plus
// one SKILL.md fetch.
// An audit verdict, when the source supplies one. The anonymous registry does
// not, so `NotAudited` is a statement about the source, never a check mark.
enum class AuditVerdict : unsigned char { NotAudited, Pass, Warn, Fail };

const char* audit_verdict_name(AuditVerdict verdict) noexcept;
AuditVerdict audit_verdict_from(const std::string& text) noexcept;

struct SkillDetail {
    SkillRef ref;
    std::string description;
    std::string license;
    std::string category;
    std::string default_branch;
    std::string skill_dir; // repo-relative directory; empty at repo root
    std::string hash;      // SKILL.md blob SHA
    // Sanitized SKILL.md text. The preview and the risk scan read this; it is
    // remote text, so it is sanitized at the adapter boundary like every other
    // remote string.
    std::string body;
    std::vector<SkillAsset> assets;
    // Optional audit, filled by a source that has one (v1 adapter, a mirror).
    AuditVerdict audit = AuditVerdict::NotAudited;
    std::string audit_note;
};

struct DetailResult {
    std::optional<SkillDetail> detail;
    IndexFailure error;

    bool ok() const noexcept { return detail.has_value(); }
};

// One file's bytes, keyed by the path the installer writes.
struct SkillAssetContent {
    std::string path;
    std::string contents;
};

struct AssetsResult {
    std::optional<std::vector<SkillAssetContent>> files;
    IndexFailure error;

    bool ok() const noexcept { return files.has_value(); }
};

// A sliding-window request budget shared by every adapter that talks to one
// host. Enabling the authenticated v1 API must not double the anonymous
// adapter's allowance, so both consume from the same instance.
class RateBudget {
public:
    explicit RateBudget(int per_minute = 600);

    // Consume one request when the window has room.
    bool consume(long long now_ms) const;
    // Whole seconds until the oldest request leaves the window.
    int retry_after_s(long long now_ms) const;

private:
    int per_minute_;
    mutable std::mutex mutex_;
    mutable std::deque<long long> hits_;
};

struct SkillIndexOptions {
    // Empty disables the adapter entirely (skills_index = none).
    std::string base_url = "https://skills.sh";
    // Empty means $XDG_CACHE_HOME/amber/skill-index.
    std::string cache_root;
    // Optional bearer for the v1 API; the unauthenticated search ignores it.
    std::string token;
    // The variable the token came from, named in the one-line rejection note.
    // Never the token itself.
    std::string token_label = "SKILLS_SH_TOKEN";
    // Shared by every adapter over this host; null means unlimited.
    std::shared_ptr<RateBudget> budget;
    long timeout_s = 15;
    long long cache_ttl_ms = 60LL * 1000;
    // One backoff retry after a 5xx or a timeout. Zero retries immediately
    // (what a suite that asserts call counts wants); negative disables it.
    int retry_delay_ms = 200;
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

    // The editorial set a source publishes. The anonymous search has none, so
    // the default says so rather than inventing one.
    virtual IndexResult curated(int limit, const CancellationToken& cancel) const;

    // Detail is adapter-specific: search returns id/source/name/installs, and
    // only an adapter that can read the source repository can answer the rest.
    // The default says so rather than pretending an empty detail.
    virtual DetailResult detail(const SkillRef& ref, const CancellationToken& cancel) const;

    // Fetch every asset a detail listed. The installer owns validation and
    // atomic writes; the adapter owns the bytes.
    virtual AssetsResult fetch_assets(const SkillDetail& detail,
                                      const CancellationToken& cancel) const;

    virtual std::vector<SourceStatus> sources() const = 0;

    // One line about a degraded source that still answers (a rejected token
    // falling back to the anonymous search). Empty when there is nothing to
    // say. The UI shows it once, not per keystroke.
    virtual std::string status_note() const { return {}; }
};

// RFC 3986 component encoding, uppercase hex. Shared by the adapters so the
// search URL and a repository path cannot disagree about a space.
std::string url_encode_component(const std::string& in);

// Map an HTTP outcome onto the typed failure. `None` means a 2xx response; a
// 429 carries Retry-After. Adapters add their own rate-limit dialects on top.
IndexFailure classify_http(const std::optional<HttpResponse>& response);

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

// --- Optional v1 half -----------------------------------------------------
//
// The v1 API is a different half of the same registry: bulk pages, the curated
// set, stable hashes and audit verdicts. It needs a bearer the anonymous
// adapter does not, so it is a separate adapter behind the same port and is
// only constructed when a token exists.

// "https://<base>/api/v1/skills/search?q=<q>&limit=<n>"
std::string skills_v1_search_url(const std::string& base_url, const std::string& query, int limit);
// "https://<base>/api/v1/skills/curated?limit=<n>"
std::string skills_v1_curated_url(const std::string& base_url, int limit);
// "https://<base>/api/v1/skills/<id>" (each id segment encoded, '/' kept)
std::string skills_v1_detail_url(const std::string& base_url, const std::string& id);
// "https://<base>/api/v1/skills/audit/<id>"
std::string skills_v1_audit_url(const std::string& base_url, const std::string& id);

// The page shape shared by the v1 bulk endpoints: `{skills:[...], count}` with
// the same required fields as the anonymous search. Extra fields (description,
// hash, audit) are carried into the detail, not the row.
IndexResult parse_skills_v1_page(const std::string& body, int limit, bool cached);

// One skill's v1 detail. `body` and `files` are optional; a response without
// them still yields description/hash/audit.
DetailResult parse_skills_v1_detail(const std::string& body, const SkillRef& ref);

// Fill `detail.audit`/`audit_note` from the `/audit/<id>` shape. Returns the
// failure when the body is not that shape.
IndexFailure parse_skills_v1_audit(const std::string& body, SkillDetail& detail);

class SkillsShV1Index final : public SkillIndex {
public:
    explicit SkillsShV1Index(SkillIndexOptions opts);

    IndexResult search(const std::string& query, int limit,
                       const CancellationToken& cancel) const override;
    IndexResult curated(int limit, const CancellationToken& cancel) const override;
    DetailResult detail(const SkillRef& ref, const CancellationToken& cancel) const override;
    std::vector<SourceStatus> sources() const override;

private:
    DetailResult fetch_detail(const SkillRef& ref, const CancellationToken& cancel) const;
    // The detail carried no verdict: ask the audit endpoint once. A failed
    // audit keeps the detail and reports why in `error`.
    void enrich_audit(SkillDetail& detail, const SkillRef& ref, long long now, IndexFailure& error,
                      const CancellationToken& cancel) const;
    // One GET with the bearer, the shared budget and the typed failure map.
    std::optional<HttpResponse> get(const std::string& url, const CancellationToken& cancel) const;
    IndexResult page(const std::string& url, int limit, const CancellationToken& cancel) const;

    SkillIndexOptions opts_;
    mutable long long last_fetch_ms_ = 0;
};

// The authenticated adapter in front of the anonymous one. A rejected token is
// not an outage: it is remembered once, reported in one line, and every search
// is answered by the anonymous adapter instead. Detail prefers the v1 metadata
// (hash, audit) and takes the body from the GitHub adapter when one is wired.
class SkillsShTokenIndex final : public SkillIndex {
public:
    SkillsShTokenIndex(std::shared_ptr<SkillIndex> anonymous, std::shared_ptr<SkillIndex> v1,
                       std::shared_ptr<SkillIndex> body,
                       std::string token_label = "SKILLS_SH_TOKEN");

    IndexResult search(const std::string& query, int limit,
                       const CancellationToken& cancel) const override;
    IndexResult curated(int limit, const CancellationToken& cancel) const override;
    DetailResult detail(const SkillRef& ref, const CancellationToken& cancel) const override;
    AssetsResult fetch_assets(const SkillDetail& detail,
                              const CancellationToken& cancel) const override;
    std::vector<SourceStatus> sources() const override;

    // One line naming the variable when the token was rejected, empty
    // otherwise. Reading it does not clear the state: the UI shows the note
    // when it changes, and every later search still knows the token is dead.
    std::string rejection_note() const;
    std::string status_note() const override { return rejection_note(); }

private:
    bool token_rejected() const;
    std::shared_ptr<SkillIndex> anonymous_;
    std::shared_ptr<SkillIndex> v1_;
    std::shared_ptr<SkillIndex> body_;
    std::string token_label_;
    mutable std::mutex note_mutex_;
    mutable bool rejected_ = false;
};

// The adapter the host should hold: the anonymous one when no token is set,
// otherwise the token-fronted composite. `body` supplies detail bodies and
// assets (the GitHub adapter in production) and may be null.
std::shared_ptr<SkillIndex> make_skills_sh_index(const SkillIndexOptions& opts,
                                                 std::shared_ptr<SkillIndex> body = nullptr);

} // namespace agent

#endif // AGENT_SKILL_INDEX_H
