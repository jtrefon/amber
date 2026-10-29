#include "agent/github_index.h"

#include "agent/remote_text.h"
#include "agent/skill_file.h"
#include "agent/skill_install.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <system_error>

namespace agent {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr size_t kMaxAssets = 200;
constexpr long long kMaxAssetBytes = 10LL * 1024 * 1024;

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long clock_ms(const GithubOptions& o) {
    return o.now_ms ? o.now_ms() : wall_ms();
}

std::string strip_slashes(std::string s) {
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

std::string field(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return {};
    return sanitize_remote_one_line(j[key].get<std::string>());
}

std::string header(const HttpResponse& r, const char* key) {
    const auto it = r.headers.find(key);
    return it == r.headers.end() ? std::string() : it->second;
}

bool safe_path(const std::string& p) {
    return !p.empty() && p.front() == '/' && p.find("..") == std::string::npos;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

struct RepoFile {
    std::string path;
    size_t bytes = 0;
    std::string sha;
};

struct RepoBundle {
    long long fetched_at_ms = 0;
    std::string branch;
    std::string license;
    std::string category;
    std::vector<RepoFile> files;
};

json bundle_json(const RepoBundle& b) {
    json files = json::array();
    for (const auto& f : b.files)
        files.push_back({{"path", f.path}, {"bytes", f.bytes}, {"sha", f.sha}});
    return {{"fetched_at_ms", b.fetched_at_ms},
            {"branch", b.branch},
            {"license", b.license},
            {"category", b.category},
            {"files", std::move(files)}};
}

bool read_files(const json& j, RepoBundle& b) {
    if (!j.contains("files") || !j["files"].is_array())
        return false;
    for (const auto& f : j["files"]) {
        if (!f.is_object() || !f.contains("path") || !f["path"].is_string())
            continue;
        RepoFile file;
        file.path = f["path"].get<std::string>();
        file.bytes = f.value("bytes", 0);
        file.sha = f.value("sha", std::string());
        if (!file.path.empty())
            b.files.push_back(std::move(file));
    }
    return true;
}

std::optional<RepoBundle> bundle_from_json(const json& j) {
    if (!j.is_object() || !j.contains("fetched_at_ms") || !j["fetched_at_ms"].is_number_integer())
        return std::nullopt;
    RepoBundle b;
    b.fetched_at_ms = j["fetched_at_ms"].get<long long>();
    b.branch = j.value("branch", std::string());
    b.license = j.value("license", std::string());
    b.category = j.value("category", std::string());
    if (b.branch.empty() || !read_files(j, b))
        return std::nullopt;
    return b;
}

std::string repo_cache_path(const GithubOptions& o, const std::string& source) {
    const std::string root = o.cache_root.empty() ? skills_index_cache_dir() : o.cache_root;
    char name[48];
    std::snprintf(name, sizeof(name), "repo-%016llx.json",
                  static_cast<unsigned long long>(fnv1a(source)));
    return root + "/" + name;
}

std::optional<RepoBundle> load_bundle(const GithubOptions& o, const std::string& source) {
    const std::string path = repo_cache_path(o, source);
    if (!safe_path(path))
        return std::nullopt;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded())
        return std::nullopt;
    return bundle_from_json(j);
}

void save_bundle(const GithubOptions& o, const std::string& source, const RepoBundle& b) {
    const std::string path = repo_cache_path(o, source);
    const std::string tmp = path + ".tmp";
    if (!safe_path(path) || !safe_path(tmp))
        return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec)
        return;
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            return;
        f << bundle_json(b).dump();
        if (!f)
            return;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
}

std::optional<HttpResponse> call(const GithubOptions& o, const std::string& url,
                                 const CancellationToken& cancel) {
    const HttpGet get = o.http ? o.http : HttpGet(http_get);
    try {
        return get(url, o.token, o.timeout_s, cancel);
    } catch (...) {
        return std::nullopt;
    }
}

IndexFailure github_failure(const std::optional<HttpResponse>& r, long long now) {
    IndexFailure f = classify_http(r);
    if (!r || f.kind == IndexError::None)
        return f;
    if (r->status == 403 && header(*r, "x-ratelimit-remaining") == "0") {
        const long long reset = std::atoll(header(*r, "x-ratelimit-reset").c_str());
        f.kind = IndexError::RateLimited;
        f.status = 403;
        f.retry_after_s = reset > now / 1000 ? static_cast<int>(reset - now / 1000) : 0;
        f.detail = "github rate limit";
    }
    if (r->status == 404)
        f.detail = "not found";
    return f;
}

bool fetch_json(const GithubOptions& o, const std::string& url, const CancellationToken& cancel,
                long long now, json& out, IndexFailure& err) {
    const auto r = call(o, url, cancel);
    err = github_failure(r, now);
    if (err.kind != IndexError::None)
        return false;
    if (!r) {
        err = {IndexError::Offline, 0, 0, "no response"};
        return false;
    }
    out = json::parse(r->body, nullptr, false);
    if (out.is_discarded()) {
        err = {IndexError::Malformed, 0, 0, "response is not JSON"};
        return false;
    }
    return true;
}

std::string repo_api(const GithubOptions& o, const std::string& owner, const std::string& repo) {
    return strip_slashes(o.api_base) + "/repos/" + owner + "/" + repo;
}

std::string tree_api(const GithubOptions& o, const std::string& owner, const std::string& repo,
                     const std::string& branch) {
    return repo_api(o, owner, repo) + "/git/trees/" + url_encode_component(branch) + "?recursive=1";
}

std::string raw_url(const GithubOptions& o, const std::string& owner, const std::string& repo,
                    const std::string& branch, const std::string& path) {
    return strip_slashes(o.raw_base) + "/" + owner + "/" + repo + "/" + branch + "/" + path;
}

bool parse_tree(const json& j, std::vector<RepoFile>& out) {
    if (!j.contains("tree") || !j["tree"].is_array())
        return false;
    if (j.value("truncated", false))
        return false;
    for (const auto& entry : j["tree"]) {
        if (!entry.is_object() || field(entry, "type") != "blob")
            continue;
        RepoFile f;
        f.path = field(entry, "path");
        f.sha = field(entry, "sha");
        f.bytes = entry.value("size", 0);
        if (f.path.empty())
            continue;
        out.push_back(std::move(f));
    }
    return true;
}

std::string marketplace_category(const json& j) {
    if (j.contains("plugins") && j["plugins"].is_array() && !j["plugins"].empty()) {
        const auto& plugin = j["plugins"][0];
        std::string category = field(plugin, "category");
        if (!category.empty())
            return category;
        if (plugin.contains("keywords") && plugin["keywords"].is_array() &&
            !plugin["keywords"].empty() && plugin["keywords"][0].is_string())
            return sanitize_remote_one_line(plugin["keywords"][0].get<std::string>());
    }
    return field(j, "category");
}

std::string fetch_marketplace(const GithubOptions& o, const std::string& owner,
                              const std::string& repo, const std::string& branch,
                              const CancellationToken& cancel) {
    const std::string url = raw_url(o, owner, repo, branch, ".claude-plugin/marketplace.json");
    const auto r = call(o, url, cancel);
    if (!r || r->status != 200)
        return {};
    const json j = json::parse(r->body, nullptr, false);
    return j.is_discarded() ? std::string() : marketplace_category(j);
}

std::optional<RepoBundle> fetch_bundle(const GithubOptions& o, const std::string& owner,
                                       const std::string& repo, const std::string& source,
                                       const CancellationToken& cancel, IndexFailure& err) {
    const long long now = clock_ms(o);
    json meta;
    if (!fetch_json(o, repo_api(o, owner, repo), cancel, now, meta, err))
        return std::nullopt;

    RepoBundle b;
    b.fetched_at_ms = now;
    b.branch = field(meta, "default_branch");
    if (b.branch.empty()) {
        err = {IndexError::Malformed, 0, 0, "repository has no default branch"};
        return std::nullopt;
    }
    if (meta.contains("license") && meta["license"].is_object())
        b.license = field(meta["license"], "spdx_id");

    json tree;
    if (!fetch_json(o, tree_api(o, owner, repo, b.branch), cancel, now, tree, err))
        return std::nullopt;
    if (!parse_tree(tree, b.files)) {
        err = {IndexError::Malformed, 0, 0, "repository tree is unreadable or truncated"};
        return std::nullopt;
    }
    b.category = fetch_marketplace(o, owner, repo, b.branch, cancel);
    save_bundle(o, source, b);
    return b;
}

std::string parent_dir(const std::string& path) {
    const auto pos = path.rfind('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string base_name(const std::string& path) {
    const auto pos = path.rfind('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool is_skill_md(const std::string& path) {
    return base_name(path) == "SKILL.md";
}

std::optional<std::string> choose_skill_dir(const RepoBundle& b, const std::string& slug) {
    std::optional<std::string> best;
    for (const auto& f : b.files) {
        if (!is_skill_md(f.path) || base_name(parent_dir(f.path)) != slug)
            continue;
        if (!best || f.path.size() < best->size())
            best = parent_dir(f.path);
    }
    if (best)
        return best;

    // A repository-level SKILL.md is unambiguous: there is no directory to name
    // after the slug. A differently named nested skill is not guessed at --
    // returning "some skill" is worse than saying the slug was not found.
    for (const auto& f : b.files) {
        if (is_skill_md(f.path) && parent_dir(f.path).empty())
            return std::string();
    }
    return std::nullopt;
}

std::string skill_path(const std::string& dir) {
    return dir.empty() ? "SKILL.md" : dir + "/SKILL.md";
}

std::string category_from(const SkillMeta& meta, const std::string& fallback) {
    if (meta.metadata.is_object() && meta.metadata.contains("category") &&
        meta.metadata["category"].is_string())
        return sanitize_remote_one_line(meta.metadata["category"].get<std::string>());
    return fallback;
}

std::vector<SkillAsset> assets_under(const RepoBundle& b, const std::string& dir) {
    const std::string prefix = dir.empty() ? std::string() : dir + "/";
    std::vector<SkillAsset> out;
    for (const auto& f : b.files) {
        if (f.path.size() < prefix.size() || f.path.compare(0, prefix.size(), prefix) != 0)
            continue;
        out.push_back({f.path.substr(prefix.size()), f.bytes, f.sha});
    }
    return out;
}

std::string hash_of(const RepoBundle& b, const std::string& path) {
    for (const auto& f : b.files) {
        if (f.path == path)
            return f.sha;
    }
    return {};
}

struct BundleOutcome {
    std::optional<RepoBundle> bundle;
    IndexFailure error;
    bool fetched = false;
};

BundleOutcome resolve_bundle(const GithubOptions& o, const std::string& source,
                             const std::string& owner, const std::string& repo,
                             const CancellationToken& cancel) {
    const long long now = clock_ms(o);
    auto bundle = load_bundle(o, source);
    if (bundle && now - bundle->fetched_at_ms < o.repo_ttl_ms)
        return {std::move(bundle), {}, false};

    IndexFailure err;
    auto fresh = fetch_bundle(o, owner, repo, source, cancel, err);
    if (fresh)
        return {std::move(fresh), {}, true};
    if (bundle)
        return {std::move(bundle), {}, false};
    return {std::nullopt, std::move(err), false};
}

DetailResult assemble_detail(const GithubOptions& o, const SkillRef& ref, const RepoBundle& bundle,
                             const std::string& owner, const std::string& repo,
                             const CancellationToken& cancel) {
    const std::string slug = skill_slug(ref.id);
    const auto dir = choose_skill_dir(bundle, slug);
    if (!dir)
        return {std::nullopt, {IndexError::Malformed, 0, 0, "no SKILL.md for this skill"}};

    const std::string path = skill_path(*dir);
    const auto r = call(o, raw_url(o, owner, repo, bundle.branch, path), cancel);
    const IndexFailure failure = github_failure(r, clock_ms(o));
    if (failure.kind != IndexError::None)
        return {std::nullopt, failure};
    if (!r)
        return {std::nullopt, {IndexError::Offline, 0, 0, "no response"}};

    const auto meta = parse_skill_meta(r->body);
    if (!meta)
        return {std::nullopt, {IndexError::Malformed, 0, 0, "SKILL.md frontmatter did not parse"}};

    SkillDetail d;
    d.ref = ref;
    d.body = sanitize_remote_text(r->body);
    if (!meta->name.empty())
        d.ref.name = sanitize_remote_one_line(meta->name);
    d.description = sanitize_remote_one_line(meta->description);
    d.license = meta->license.empty() ? bundle.license : sanitize_remote_one_line(meta->license);
    d.category = category_from(*meta, bundle.category);
    d.default_branch = bundle.branch;
    d.skill_dir = *dir;
    d.hash = hash_of(bundle, path);
    d.assets = assets_under(bundle, *dir);
    return {std::move(d), {}};
}

} // namespace

bool split_github_source(const std::string& source, std::string& owner, std::string& repo) {
    const auto slash = source.find('/');
    if (slash == std::string::npos || source.find('/', slash + 1) != std::string::npos)
        return false;
    owner = source.substr(0, slash);
    repo = source.substr(slash + 1);

    const auto valid = [](const std::string& s) {
        if (s.empty() || s == "." || s == "..")
            return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
        });
    };
    return valid(owner) && valid(repo);
}

std::string skill_slug(const std::string& id) {
    const auto pos = id.rfind('/');
    return pos == std::string::npos ? id : id.substr(pos + 1);
}

GitHubIndex::GitHubIndex(GithubOptions opts) : opts_(std::move(opts)) {}

std::vector<SkillRef> GitHubIndex::list_skills(const std::string& source,
                                               const CancellationToken& cancel,
                                               std::string& error) const {
    error.clear();
    std::string owner, repo;
    if (!split_github_source(source, owner, repo)) {
        error = "expected owner/repo";
        return {};
    }
    const BundleOutcome outcome = resolve_bundle(opts_, source, owner, repo, cancel);
    if (!outcome.bundle) {
        error = outcome.error.detail.empty() ? "cannot read the repository" : outcome.error.detail;
        return {};
    }
    std::vector<SkillRef> refs;
    std::set<std::string> seen;
    for (const auto& file : outcome.bundle->files) {
        if (!is_skill_md(file.path))
            continue;
        const std::string dir = parent_dir(file.path);
        SkillRef ref;
        ref.source = source;
        ref.name = dir.empty() ? repo : base_name(dir);
        ref.id = source + "/" + ref.name;
        if (seen.insert(ref.id).second)
            refs.push_back(std::move(ref));
    }
    if (refs.empty())
        error = "no SKILL.md in " + source;
    return refs;
}

IndexResult GitHubIndex::search(const std::string&, int, const CancellationToken&) const {
    return {std::nullopt, {IndexError::Unsupported, 0, 0, "github adapter does not search"}};
}

DetailResult GitHubIndex::detail(const SkillRef& ref, const CancellationToken& cancel) const {
    std::string owner;
    std::string repo;
    if (!split_github_source(ref.source, owner, repo))
        return {std::nullopt, {IndexError::Malformed, 0, 0, "source is not owner/repo"}};
    if (cancel.is_requested())
        return {std::nullopt, {IndexError::Cancelled, 0, 0, "cancelled"}};

    BundleOutcome outcome = resolve_bundle(opts_, ref.source, owner, repo, cancel);
    if (!outcome.bundle)
        return {std::nullopt, std::move(outcome.error)};
    if (outcome.fetched)
        last_fetch_ms_ = clock_ms(opts_);
    return assemble_detail(opts_, ref, *outcome.bundle, owner, repo, cancel);
}

std::vector<SourceStatus> GitHubIndex::sources() const {
    SourceStatus s;
    s.name = "github";
    s.enabled = true;
    s.authenticated = !opts_.token.empty();
    s.fetched_at_ms = last_fetch_ms_;
    return {s};
}

AssetsResult GitHubIndex::fetch_assets(const SkillDetail& detail,
                                       const CancellationToken& cancel) const {
    std::string owner;
    std::string repo;
    if (!split_github_source(detail.ref.source, owner, repo))
        return {std::nullopt, {IndexError::Malformed, 0, 0, "source is not owner/repo"}};
    if (detail.assets.empty() || detail.assets.size() > kMaxAssets)
        return {std::nullopt, {IndexError::Malformed, 0, 0, "skill has an invalid file count"}};

    long long declared = 0;
    for (const auto& asset : detail.assets)
        declared += static_cast<long long>(asset.bytes);
    if (declared > kMaxAssetBytes)
        return {std::nullopt, {IndexError::Malformed, 0, 0, "skill exceeds the size cap"}};

    std::vector<SkillAssetContent> files;
    files.reserve(detail.assets.size());
    long long total = 0;
    const long long now = clock_ms(opts_);
    for (const auto& asset : detail.assets) {
        if (cancel.is_requested())
            return {std::nullopt, {IndexError::Cancelled, 0, 0, "cancelled"}};
        if (!is_safe_skill_path(asset.path))
            return {std::nullopt, {IndexError::Malformed, 0, 0, "unsafe asset path"}};

        const std::string path =
            detail.skill_dir.empty() ? asset.path : detail.skill_dir + "/" + asset.path;
        const auto r =
            call(opts_, raw_url(opts_, owner, repo, detail.default_branch, path), cancel);
        const IndexFailure failure = github_failure(r, now);
        if (failure.kind != IndexError::None)
            return {std::nullopt, failure};
        if (!r)
            return {std::nullopt, {IndexError::Offline, 0, 0, "no response"}};
        total += static_cast<long long>(r->body.size());
        if (total > kMaxAssetBytes)
            return {std::nullopt, {IndexError::Malformed, 0, 0, "skill exceeds the size cap"}};
        files.push_back({asset.path, r->body});
    }
    return {std::move(files), {}};
}

} // namespace agent
