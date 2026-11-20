// The optional v1 half of the skills.sh registry: bulk pages, the curated set,
// stable hashes and audit verdicts. It is a separate adapter because it needs a
// bearer the anonymous search does not, and it is only constructed when the
// user supplied one.

#include "agent/skill_index.h"

#include "agent/remote_text.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

using json = nlohmann::json;

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

IndexFailure rejected(const std::string& detail, long status = 0) {
    IndexFailure f;
    f.kind = IndexError::Http;
    f.status = status;
    f.detail = detail;
    return f;
}

IndexResult unsupported(const std::string& detail) {
    IndexFailure f;
    f.kind = IndexError::Unsupported;
    f.detail = detail;
    return {std::nullopt, std::move(f)};
}

IndexResult cancelled() {
    return {std::nullopt, {IndexError::Cancelled, 0, 0, "cancelled"}};
}

IndexFailure malformed(const std::string& detail) {
    IndexFailure f;
    f.kind = IndexError::Malformed;
    f.detail = detail;
    return f;
}

std::string trimmed_base(const std::string& base_url) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base;
}

int capped_limit(int limit) {
    return std::clamp(limit <= 0 ? 20 : limit, 1, 200);
}

// A v1 detail id is a path ("owner/repo/slug"): encode each segment, keep the
// separators, so a space or a '%' cannot invent a different endpoint.
std::string encoded_id_path(const std::string& id) {
    std::string out;
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t slash = id.find('/', start);
        const std::string segment =
            id.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!out.empty())
            out.push_back('/');
        out += url_encode_component(segment);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return out;
}

std::string text_field(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return {};
    return sanitize_remote_one_line(it->get<std::string>());
}

std::string body_field(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return {};
    return sanitize_remote_text(it->get<std::string>());
}

// Consume one shared slot, or say why the caller must wait. `None` means the
// request may proceed.
IndexFailure take_budget(const SkillIndexOptions& opts, long long now) {
    if (!opts.budget)
        return {};
    if (opts.budget->consume(now))
        return {};
    IndexFailure f;
    f.kind = IndexError::RateLimited;
    f.status = 429;
    f.retry_after_s = opts.budget->retry_after_s(now);
    f.detail = "rate limited";
    return f;
}

// The v1 metadata wins where it speaks; the GitHub body and its asset paths
// stay, because the installer fetches bytes from the repository.
SkillDetail merge_detail(const SkillDetail& base, const SkillDetail& meta) {
    SkillDetail merged = base;
    if (!meta.description.empty())
        merged.description = meta.description;
    if (!meta.license.empty())
        merged.license = meta.license;
    if (!meta.category.empty())
        merged.category = meta.category;
    if (!meta.default_branch.empty())
        merged.default_branch = meta.default_branch;
    if (!meta.skill_dir.empty())
        merged.skill_dir = meta.skill_dir;
    if (!meta.hash.empty())
        merged.hash = meta.hash;
    if (!meta.body.empty())
        merged.body = meta.body;
    if (meta.audit != AuditVerdict::NotAudited) {
        merged.audit = meta.audit;
        merged.audit_note = meta.audit_note;
    }
    return merged;
}

} // namespace

RateBudget::RateBudget(int per_minute) : per_minute_(std::max(1, per_minute)) {}

bool RateBudget::consume(long long now_ms) const {
    constexpr long long kWindowMs = 60'000;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!hits_.empty() && now_ms - hits_.front() >= kWindowMs)
        hits_.pop_front();
    if (static_cast<int>(hits_.size()) >= per_minute_)
        return false;
    hits_.push_back(now_ms);
    return true;
}

int RateBudget::retry_after_s(long long now_ms) const {
    constexpr long long kWindowMs = 60'000;
    std::lock_guard<std::mutex> lock(mutex_);
    if (hits_.empty())
        return 0;
    const long long wait = kWindowMs - (now_ms - hits_.front());
    return wait <= 0 ? 0 : static_cast<int>((wait + 999) / 1000);
}

IndexResult SkillIndex::curated(int, const CancellationToken&) const {
    return unsupported("curated is not supported");
}

std::string skills_v1_search_url(const std::string& base_url, const std::string& query, int limit) {
    return trimmed_base(base_url) + "/api/v1/skills/search?q=" + url_encode_component(query) +
           "&limit=" + std::to_string(capped_limit(limit));
}

std::string skills_v1_curated_url(const std::string& base_url, int limit) {
    return trimmed_base(base_url) +
           "/api/v1/skills/curated?limit=" + std::to_string(capped_limit(limit));
}

std::string skills_v1_detail_url(const std::string& base_url, const std::string& id) {
    return trimmed_base(base_url) + "/api/v1/skills/" + encoded_id_path(id);
}

std::string skills_v1_audit_url(const std::string& base_url, const std::string& id) {
    return trimmed_base(base_url) + "/api/v1/skills/audit/" + encoded_id_path(id);
}

IndexResult parse_skills_v1_page(const std::string& body, int limit, bool cached) {
    return parse_skills_search(body, limit, cached);
}

DetailResult parse_skills_v1_detail(const std::string& body, const SkillRef& ref) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {std::nullopt, malformed("v1 detail is not a JSON object")};
    SkillDetail detail;
    detail.ref = ref;
    detail.description = text_field(j, "description");
    detail.license = text_field(j, "license");
    detail.category = text_field(j, "category");
    detail.default_branch = text_field(j, "default_branch");
    detail.skill_dir = text_field(j, "skill_dir");
    detail.hash = text_field(j, "hash");
    detail.body = body_field(j, "body");
    if (const auto files = j.find("files"); files != j.end() && files->is_array()) {
        for (const auto& entry : *files) {
            if (!entry.is_object())
                continue;
            SkillAsset asset;
            asset.path = text_field(entry, "path");
            asset.sha = text_field(entry, "sha");
            if (entry.contains("bytes") && entry["bytes"].is_number_integer())
                asset.bytes = static_cast<size_t>(std::max(0, entry["bytes"].get<int>()));
            if (!asset.path.empty())
                detail.assets.push_back(std::move(asset));
        }
    }
    if (const auto audit = j.find("audit"); audit != j.end() && audit->is_object()) {
        detail.audit = audit_verdict_from(text_field(*audit, "verdict"));
        detail.audit_note = text_field(*audit, "note");
    }
    return {std::move(detail), {}};
}

IndexFailure parse_skills_v1_audit(const std::string& body, SkillDetail& detail) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return malformed("v1 audit is not a JSON object");
    detail.audit = audit_verdict_from(text_field(j, "verdict"));
    detail.audit_note = text_field(j, "note");
    return {};
}

SkillsShV1Index::SkillsShV1Index(SkillIndexOptions opts) : opts_(std::move(opts)) {}

std::optional<HttpResponse> SkillsShV1Index::get(const std::string& url,
                                                 const CancellationToken& cancel) const {
    const HttpGet getter = opts_.http ? opts_.http : HttpGet(http_get);
    try {
        return getter(url, opts_.token, opts_.timeout_s, cancel);
    } catch (...) {
        return std::nullopt;
    }
}

IndexResult SkillsShV1Index::page(const std::string& url, int limit,
                                  const CancellationToken& cancel) const {
    if (opts_.base_url.empty())
        return unsupported("skill index disabled");
    if (opts_.token.empty())
        return unsupported("the v1 API needs a token");
    const long long now = opts_.now_ms ? opts_.now_ms() : wall_ms();
    if (IndexFailure budget = take_budget(opts_, now); budget.kind != IndexError::None)
        return {std::nullopt, budget};
    const auto response = get(url, cancel);
    if (cancel.is_requested())
        return cancelled();
    const IndexFailure live = classify_http(response);
    if (live.kind != IndexError::None)
        return {std::nullopt, live};
    if (!response)
        return {std::nullopt, rejected("no response")};
    IndexResult parsed = parse_skills_v1_page(response->body, limit, false);
    if (parsed.ok())
        last_fetch_ms_ = now;
    return parsed;
}

IndexResult SkillsShV1Index::search(const std::string& query, int limit,
                                    const CancellationToken& cancel) const {
    if (query.empty())
        return unsupported("empty query");
    if (cancel.is_requested())
        return cancelled();
    return page(skills_v1_search_url(opts_.base_url, query, limit), limit, cancel);
}

IndexResult SkillsShV1Index::curated(int limit, const CancellationToken& cancel) const {
    if (cancel.is_requested())
        return cancelled();
    return page(skills_v1_curated_url(opts_.base_url, limit), limit, cancel);
}

void SkillsShV1Index::enrich_audit(SkillDetail& detail, const SkillRef& ref, long long now,
                                   IndexFailure& error, const CancellationToken& cancel) const {
    if (detail.audit != AuditVerdict::NotAudited || cancel.is_requested())
        return;
    if (IndexFailure budget = take_budget(opts_, now); budget.kind != IndexError::None) {
        error = budget;
        return;
    }
    const auto audit = get(skills_v1_audit_url(opts_.base_url, ref.id), cancel);
    const IndexFailure audit_failure = classify_http(audit);
    if (audit_failure.kind != IndexError::None) {
        error = audit_failure;
        return;
    }
    if (audit)
        error = parse_skills_v1_audit(audit->body, detail);
}

DetailResult SkillsShV1Index::fetch_detail(const SkillRef& ref,
                                           const CancellationToken& cancel) const {
    if (opts_.base_url.empty())
        return {std::nullopt, rejected("skill index disabled")};
    if (opts_.token.empty())
        return {std::nullopt, rejected("the v1 API needs a token")};
    const long long now = opts_.now_ms ? opts_.now_ms() : wall_ms();
    if (IndexFailure budget = take_budget(opts_, now); budget.kind != IndexError::None)
        return {std::nullopt, budget};
    const auto response = get(skills_v1_detail_url(opts_.base_url, ref.id), cancel);
    if (cancel.is_requested())
        return {std::nullopt, {IndexError::Cancelled, 0, 0, "cancelled"}};
    const IndexFailure live = classify_http(response);
    if (live.kind != IndexError::None)
        return {std::nullopt, live};
    if (!response)
        return {std::nullopt, rejected("no response")};
    DetailResult parsed = parse_skills_v1_detail(response->body, ref);
    if (!parsed.ok() || !parsed.detail)
        return parsed;
    enrich_audit(*parsed.detail, ref, now, parsed.error, cancel);
    last_fetch_ms_ = now;
    return parsed;
}

DetailResult SkillsShV1Index::detail(const SkillRef& ref, const CancellationToken& cancel) const {
    return fetch_detail(ref, cancel);
}

std::vector<SourceStatus> SkillsShV1Index::sources() const {
    SourceStatus s;
    s.name = "skills.sh";
    s.enabled = !opts_.base_url.empty();
    s.authenticated = !opts_.token.empty();
    s.fetched_at_ms = last_fetch_ms_;
    return {s};
}

SkillsShTokenIndex::SkillsShTokenIndex(std::shared_ptr<SkillIndex> anonymous,
                                       std::shared_ptr<SkillIndex> v1,
                                       std::shared_ptr<SkillIndex> body, std::string token_label)
    : anonymous_(std::move(anonymous)), v1_(std::move(v1)), body_(std::move(body)),
      token_label_(std::move(token_label)) {}

bool SkillsShTokenIndex::token_rejected() const {
    std::lock_guard<std::mutex> lock(note_mutex_);
    return rejected_;
}

std::string SkillsShTokenIndex::rejection_note() const {
    std::lock_guard<std::mutex> lock(note_mutex_);
    if (!rejected_)
        return {};
    return token_label_ + " was rejected; using unauthenticated search";
}

namespace {

bool auth_failure(const IndexFailure& f) {
    return f.kind == IndexError::Http && (f.status == 401 || f.status == 403);
}

} // namespace

IndexResult SkillsShTokenIndex::search(const std::string& query, int limit,
                                       const CancellationToken& cancel) const {
    if (!v1_ || token_rejected())
        return anonymous_ ? anonymous_->search(query, limit, cancel)
                          : unsupported("no search backend");
    IndexResult result = v1_->search(query, limit, cancel);
    if (result.ok())
        return result;
    if (!auth_failure(result.error))
        return result;
    {
        std::lock_guard<std::mutex> lock(note_mutex_);
        rejected_ = true;
    }
    return anonymous_ ? anonymous_->search(query, limit, cancel) : result;
}

IndexResult SkillsShTokenIndex::curated(int limit, const CancellationToken& cancel) const {
    if (!v1_)
        return unsupported("curated is not supported");
    if (token_rejected()) {
        IndexFailure f;
        f.kind = IndexError::Http;
        f.status = 401;
        f.detail = "token rejected";
        return {std::nullopt, f};
    }
    IndexResult result = v1_->curated(limit, cancel);
    if (!result.ok() && auth_failure(result.error)) {
        std::lock_guard<std::mutex> lock(note_mutex_);
        rejected_ = true;
    }
    return result;
}

DetailResult SkillsShTokenIndex::detail(const SkillRef& ref,
                                        const CancellationToken& cancel) const {
    DetailResult base =
        body_ ? body_->detail(ref, cancel) : DetailResult{std::nullopt, IndexFailure{}};
    if (!v1_)
        return base;
    DetailResult enriched = v1_->detail(ref, cancel);
    if (!enriched.ok() || !enriched.detail) {
        if (auth_failure(enriched.error)) {
            std::lock_guard<std::mutex> lock(note_mutex_);
            rejected_ = true;
        }
        return base.ok() && base.detail ? base : enriched;
    }
    if (!base.ok() || !base.detail)
        return enriched;
    return {merge_detail(*base.detail, *enriched.detail), enriched.error};
}

AssetsResult SkillsShTokenIndex::fetch_assets(const SkillDetail& detail,
                                              const CancellationToken& cancel) const {
    if (body_)
        return body_->fetch_assets(detail, cancel);
    return v1_ ? v1_->fetch_assets(detail, cancel)
               : AssetsResult{std::nullopt, {IndexError::Unsupported, 0, 0, "no asset backend"}};
}

std::vector<SourceStatus> SkillsShTokenIndex::sources() const {
    std::vector<SourceStatus> out;
    if (v1_) {
        out = v1_->sources();
        if (token_rejected())
            for (auto& s : out)
                s.authenticated = false;
        return out;
    }
    return anonymous_ ? anonymous_->sources() : out;
}

std::shared_ptr<SkillIndex> make_skills_sh_index(const SkillIndexOptions& opts,
                                                 std::shared_ptr<SkillIndex> body) {
    if (opts.token.empty())
        return std::make_shared<SkillsShIndex>(opts);
    // The anonymous half must not carry the v1 bearer to a second endpoint.
    SkillIndexOptions anonymous_opts = opts;
    anonymous_opts.token.clear();
    auto anonymous = std::make_shared<SkillsShIndex>(std::move(anonymous_opts));
    auto v1 = std::make_shared<SkillsShV1Index>(opts);
    return std::make_shared<SkillsShTokenIndex>(std::move(anonymous), std::move(v1),
                                                std::move(body), opts.token_label);
}

} // namespace agent
