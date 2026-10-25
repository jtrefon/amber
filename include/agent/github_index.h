#ifndef AGENT_GITHUB_INDEX_H
#define AGENT_GITHUB_INDEX_H

#include <functional>
#include <string>

#include "agent/skill_index.h"

namespace agent {

// The enrichment adapter: given a `source` (owner/repo) from a search result,
// read the repository tree, the skill's SKILL.md and the repo's optional
// `.claude-plugin/marketplace.json`, and return the detail an installer needs.
//
// Discovery is the search registry's job; this adapter answers `detail` and
// returns `Unsupported` for `search` rather than inventing a second ranking.
//
// THREAD SAFETY: one instance per worker thread, like SkillsShIndex.
struct GithubOptions {
    std::string api_base = "https://api.github.com";
    std::string raw_base = "https://raw.githubusercontent.com";
    std::string token; // optional; raises 60/h to 5,000/h
    long timeout_s = 15;
    long long repo_ttl_ms = 3600LL * 1000;
    std::string cache_root; // empty -> skills_index_cache_dir()
    HttpGet http;
    std::function<long long()> now_ms;
};

class GitHubIndex final : public SkillIndex {
public:
    explicit GitHubIndex(GithubOptions opts);

    IndexResult search(const std::string& query, int limit,
                       const CancellationToken& cancel) const override;
    DetailResult detail(const SkillRef& ref, const CancellationToken& cancel) const override;
    AssetsResult fetch_assets(const SkillDetail& detail,
                              const CancellationToken& cancel) const override;
    std::vector<SourceStatus> sources() const override;

private:
    GithubOptions opts_;
    mutable long long last_fetch_ms_ = 0;
};

// "owner/repo", exactly one slash and conservative GitHub segments. Exposed so
// the adapter's input validation is unit-tested, not inferred from a failure.
bool split_github_source(const std::string& source, std::string& owner, std::string& repo);

// "owner/repo/slug" -> "slug". The search id is the stable key.
std::string skill_slug(const std::string& id);

} // namespace agent

#endif // AGENT_GITHUB_INDEX_H
