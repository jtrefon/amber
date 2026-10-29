#include "agent/skill_index.h"

#include "agent/config.h"
#include "agent/remote_text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace agent {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

IndexFailure failure(IndexError kind, const std::string& detail, long status = 0,
                     int retry_after_s = 0) {
    IndexFailure f;
    f.kind = kind;
    f.status = status;
    f.retry_after_s = retry_after_s;
    f.detail = detail;
    return f;
}

IndexResult failed(IndexFailure f) {
    return {std::nullopt, std::move(f)};
}

bool unreserved(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string encode_component(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (const unsigned char c : in) {
        if (unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string cache_root(const SkillIndexOptions& opts) {
    return opts.cache_root.empty() ? skills_index_cache_dir() : opts.cache_root;
}

std::string cache_path(const SkillIndexOptions& opts, const std::string& query, int limit) {
    char name[48];
    const std::string key = opts.base_url + "\n" + query + "\n" + std::to_string(limit);
    std::snprintf(name, sizeof(name), "search-%016llx.json",
                  static_cast<unsigned long long>(fnv1a(key)));
    return cache_root(opts) + "/" + name;
}

struct CachedPage {
    long long fetched_at_ms = 0;
    std::string body;
};

// The cache root comes from the user's environment (XDG). A path that is not
// absolute or contains a traversal segment is refused before it reaches a file
// API; the cache is an optimisation, never a reason to touch an unexpected
// location.
bool is_safe_cache_path(const std::string& path) {
    return !path.empty() && path.front() == '/' && path.find("..") == std::string::npos;
}

std::optional<CachedPage> read_cache(const std::string& path) {
    if (!is_safe_cache_path(path))
        return std::nullopt;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    if (!j.contains("fetched_at_ms") || !j["fetched_at_ms"].is_number_integer() ||
        !j.contains("body") || !j["body"].is_string())
        return std::nullopt;
    return CachedPage{j["fetched_at_ms"].get<long long>(), j["body"].get<std::string>()};
}

void write_cache(const std::string& path, long long now, const std::string& body) {
    const std::string tmp = path + ".tmp";
    if (!is_safe_cache_path(path) || !is_safe_cache_path(tmp))
        return;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec)
        return;

    const json j{{"fetched_at_ms", now}, {"body", body}};
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            return;
        f << j.dump();
        if (!f)
            return;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
}

int retry_after_seconds(const HttpResponse& r) {
    const auto it = r.headers.find("retry-after");
    if (it == r.headers.end())
        return 0;
    return std::atoi(it->second.c_str());
}

IndexFailure classify(const std::optional<HttpResponse>& r) {
    if (!r)
        return failure(IndexError::Offline, "no response");
    if (r->status == 0)
        return failure(r->timed_out ? IndexError::Timeout : IndexError::Offline,
                       r->timed_out ? "timed out" : "transport failure");
    if (r->status == 429)
        return failure(IndexError::RateLimited, "rate limited", 429, retry_after_seconds(*r));
    if (r->status < 200 || r->status >= 300)
        return failure(IndexError::Http, "registry returned HTTP " + std::to_string(r->status),
                       r->status);
    return {};
}

// A page inside the TTL answers without a request. Returning nullopt means
// "go live", not "nothing found".
std::optional<IndexResult> fresh_cache(const std::optional<CachedPage>& cached, int limit,
                                       long long now, long long ttl) {
    if (!cached || now - cached->fetched_at_ms >= ttl)
        return std::nullopt;
    IndexResult parsed = parse_skills_search(cached->body, limit, true);
    if (!parsed.page)
        return std::nullopt;
    return parsed;
}

// A failed live request still has something to show if a page was cached: the
// user gets the rows and the error, and the header can say `stale`. Pretending
// the page is live is the failure this exists to prevent.
IndexResult fallback_to_cache(const std::optional<CachedPage>& cached, int limit, IndexFailure f) {
    if (cached) {
        IndexResult parsed = parse_skills_search(cached->body, limit, true);
        if (parsed.page) {
            parsed.page->stale = true;
            parsed.error = std::move(f);
            return parsed;
        }
    }
    return failed(std::move(f));
}

} // namespace

std::string skills_index_cache_dir() {
    return cache_dir() + "/skill-index";
}

std::string url_encode_component(const std::string& in) {
    return encode_component(in);
}

IndexFailure classify_http(const std::optional<HttpResponse>& response) {
    return classify(response);
}

const char* audit_verdict_name(AuditVerdict verdict) noexcept {
    switch (verdict) {
    case AuditVerdict::NotAudited:
        return "not audited";
    case AuditVerdict::Pass:
        return "pass";
    case AuditVerdict::Warn:
        return "warn";
    case AuditVerdict::Fail:
        return "fail";
    }
    return "not audited";
}

AuditVerdict audit_verdict_from(const std::string& text) noexcept {
    if (text == "pass" || text == "passed")
        return AuditVerdict::Pass;
    if (text == "warn" || text == "warning" || text == "warned")
        return AuditVerdict::Warn;
    if (text == "fail" || text == "failed")
        return AuditVerdict::Fail;
    return AuditVerdict::NotAudited;
}

DetailResult SkillIndex::detail(const SkillRef&, const CancellationToken&) const {
    return {std::nullopt, failure(IndexError::Unsupported, "detail is not supported")};
}

AssetsResult SkillIndex::fetch_assets(const SkillDetail&, const CancellationToken&) const {
    return {std::nullopt, failure(IndexError::Unsupported, "assets are not supported")};
}

std::string skills_search_url(const std::string& base_url, const std::string& query, int limit) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    const int capped = std::clamp(limit <= 0 ? 20 : limit, 1, 200);
    return base + "/api/search?q=" + encode_component(query) + "&limit=" + std::to_string(capped);
}

namespace {

std::string entry_text(const json& entry, const char* key) {
    if (!entry.contains(key) || !entry[key].is_string())
        return {};
    return sanitize_remote_one_line(entry[key].get<std::string>());
}

std::optional<SkillRef> parse_entry(const json& entry) {
    if (!entry.is_object())
        return std::nullopt;

    SkillRef ref;
    ref.id = entry_text(entry, "id");
    ref.name = entry_text(entry, "name");
    ref.source = entry_text(entry, "source");
    if (ref.id.empty() || ref.name.empty() || ref.source.empty())
        return std::nullopt;

    if (entry.contains("installs") && entry["installs"].is_number_integer())
        ref.installs = std::max(0, entry["installs"].get<int>());
    return ref;
}

int total_from(const json& j, size_t parsed) {
    const bool has_count = j.contains("count") && j["count"].is_number_integer();
    const int total = has_count ? j["count"].get<int>() : static_cast<int>(parsed);
    return std::max(total, static_cast<int>(parsed));
}

} // namespace

IndexResult parse_skills_search(const std::string& body, int limit, bool cached) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return failed(failure(IndexError::Malformed, "response is not a JSON object"));

    const auto it = j.find("skills");
    if (it == j.end() || !it->is_array())
        return failed(failure(IndexError::Malformed, "response has no skills array"));

    SkillPage page;
    page.cached = cached;
    const int cap = std::clamp(limit <= 0 ? 20 : limit, 1, 200);

    for (const auto& entry : *it) {
        if (static_cast<int>(page.skills.size()) >= cap)
            break;
        if (auto ref = parse_entry(entry))
            page.skills.push_back(std::move(*ref));
    }

    page.total = total_from(j, page.skills.size());
    return {std::move(page), {}};
}

SkillsShIndex::SkillsShIndex(SkillIndexOptions opts) : opts_(std::move(opts)) {}

IndexResult SkillsShIndex::search(const std::string& query, int limit,
                                  const CancellationToken& cancel) const {
    if (opts_.base_url.empty())
        return failed(failure(IndexError::Disabled, "skill index disabled"));
    if (query.empty())
        return failed(failure(IndexError::Malformed, "empty query"));
    if (cancel.is_requested())
        return failed(failure(IndexError::Cancelled, "cancelled"));

    const int capped = std::clamp(limit <= 0 ? 20 : limit, 1, 200);
    const long long now = opts_.now_ms ? opts_.now_ms() : wall_ms();
    const auto cached = read_cache(cache_path(opts_, query, capped));

    if (auto fresh = fresh_cache(cached, capped, now, opts_.cache_ttl_ms))
        return *fresh;

    IndexResult live = fetch(query, capped, now, cancel);
    if (live.ok())
        return live;
    return fallback_to_cache(cached, capped, std::move(live.error));
}

IndexResult SkillsShIndex::fetch(const std::string& query, int limit, long long now,
                                 const CancellationToken& cancel) const {
    const HttpGet get = opts_.http ? opts_.http : HttpGet(http_get);
    std::optional<HttpResponse> response;
    try {
        response = get(skills_search_url(opts_.base_url, query, limit), opts_.token,
                       opts_.timeout_s, cancel);
    } catch (...) {
        response = std::nullopt;
    }
    if (cancel.is_requested())
        return failed(failure(IndexError::Cancelled, "cancelled"));

    const IndexFailure live = classify(response);
    if (live.kind != IndexError::None)
        return failed(live);
    if (!response)
        return failed(failure(IndexError::Offline, "no response"));

    IndexResult parsed = parse_skills_search(response->body, limit, false);
    if (!parsed.page)
        return failed(parsed.error);

    write_cache(cache_path(opts_, query, limit), now, response->body);
    last_fetch_ms_ = now;
    return parsed;
}

std::vector<SourceStatus> SkillsShIndex::sources() const {
    SourceStatus s;
    s.name = "skills.sh";
    s.enabled = !opts_.base_url.empty();
    s.authenticated = !opts_.token.empty();
    s.fetched_at_ms = last_fetch_ms_;
    return {s};
}

} // namespace agent
