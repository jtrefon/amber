#include "agent/github_index.h"
#include "tests/test_util.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;

struct TempDir {
    explicit TempDir(const std::string& name) : path("/tmp/amber_github_" + name) {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string path;
};

agent::HttpResponse reply(long status, std::string body,
                          std::map<std::string, std::string> headers = {}) {
    agent::HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    r.headers = std::move(headers);
    return r;
}

struct Router {
    std::map<std::string, agent::HttpResponse> routes;
    std::map<std::string, int> calls;

    std::optional<agent::HttpResponse> get(const std::string& url) {
        ++calls[url];
        const auto it = routes.find(url);
        if (it == routes.end())
            return std::nullopt;
        return it->second;
    }
};

agent::GithubOptions options(Router& router, const std::string& cache_root) {
    agent::GithubOptions o;
    o.cache_root = cache_root;
    o.now_ms = [] { return 1000LL; };
    o.http = [&router](const std::string& url, const std::string&, long,
                       const agent::CancellationToken&) { return router.get(url); };
    return o;
}

const std::string kRepoUrl = "https://api.github.com/repos/acme/skills";
const std::string kTreeUrl = "https://api.github.com/repos/acme/skills/git/trees/main?recursive=1";
const std::string kMarketUrl =
    "https://raw.githubusercontent.com/acme/skills/main/.claude-plugin/marketplace.json";
const std::string kSkillUrl =
    "https://raw.githubusercontent.com/acme/skills/main/skills/tdd/SKILL.md";

const char* kRepoJson = R"({"default_branch":"main","license":{"spdx_id":"MIT"}})";
const char* kTreeJson = R"({"truncated":false,"tree":[
  {"path":"README.md","type":"blob","size":10,"sha":"readme"},
  {"path":"skills/tdd/SKILL.md","type":"blob","size":100,"sha":"aaa"},
  {"path":"skills/tdd/tests.md","type":"blob","size":10,"sha":"bbb"},
  {"path":"skills/other/SKILL.md","type":"blob","size":100,"sha":"ccc"}]})";
const char* kMarketJson = R"({"plugins":[{"category":"engineering"}]})";
const char* kSkillMd = "---\nname: tdd\ndescription: Test-driven development.\n---\n# TDD\n";

agent::SkillRef tdd_ref() {
    agent::SkillRef ref;
    ref.id = "acme/skills/tdd";
    ref.name = "tdd";
    ref.source = "acme/skills";
    ref.installs = 10;
    return ref;
}

} // namespace

TEST(github_split_source) {
    std::string owner;
    std::string repo;
    ASSERT_TRUE(agent::split_github_source("acme/skills", owner, repo));
    ASSERT_EQ(owner, std::string("acme"));
    ASSERT_EQ(repo, std::string("skills"));

    ASSERT_TRUE(!agent::split_github_source("acme", owner, repo));
    ASSERT_TRUE(!agent::split_github_source("acme/skills/extra", owner, repo));
    ASSERT_TRUE(!agent::split_github_source("../skills", owner, repo));
    ASSERT_TRUE(!agent::split_github_source("acme/", owner, repo));
    ASSERT_TRUE(!agent::split_github_source("a b/c", owner, repo));
}

TEST(github_skill_slug) {
    ASSERT_EQ(agent::skill_slug("acme/skills/tdd"), std::string("tdd"));
    ASSERT_EQ(agent::skill_slug("tdd"), std::string("tdd"));
}

TEST(github_detail_reads_tree_and_frontmatter) {
    TempDir tmp("nested");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, kTreeJson);
    router.routes[kMarketUrl] = reply(200, kMarketJson);
    router.routes[kSkillUrl] = reply(200, kSkillMd);

    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(tdd_ref(), {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.detail->description, std::string("Test-driven development."));
    ASSERT_EQ(r.detail->category, std::string("engineering"));
    ASSERT_EQ(r.detail->license, std::string("MIT"));
    ASSERT_EQ(r.detail->default_branch, std::string("main"));
    ASSERT_EQ(r.detail->skill_dir, std::string("skills/tdd"));
    ASSERT_EQ(r.detail->hash, std::string("aaa"));
    ASSERT_EQ(static_cast<int>(r.detail->assets.size()), 2);
    ASSERT_EQ(r.detail->assets[0].path, std::string("SKILL.md"));
    ASSERT_EQ(r.detail->assets[1].path, std::string("tests.md"));
}

TEST(github_frontmatter_category_wins) {
    TempDir tmp("category");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, kTreeJson);
    router.routes[kMarketUrl] = reply(200, kMarketJson);
    router.routes[kSkillUrl] =
        reply(200, "---\nname: tdd\ndescription: X\nmetadata:\n  category: testing\n---\n");

    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(tdd_ref(), {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.detail->category, std::string("testing"));
}

TEST(github_root_skill) {
    TempDir tmp("root");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, R"({"truncated":false,"tree":[
          {"path":"SKILL.md","type":"blob","size":100,"sha":"root"}]})");
    router.routes["https://raw.githubusercontent.com/acme/skills/main/SKILL.md"] =
        reply(200, "---\nname: acme\ndescription: Root skill.\n---\n");

    agent::SkillRef ref = tdd_ref();
    ref.id = "acme/skills/acme";
    ref.name = "acme";
    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(ref, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.detail->skill_dir, std::string(""));
    ASSERT_EQ(r.detail->hash, std::string("root"));
}

TEST(github_missing_slug_is_an_error) {
    TempDir tmp("missing");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, R"({"truncated":false,"tree":[
          {"path":"skills/other/SKILL.md","type":"blob","size":1,"sha":"a"},
          {"path":"skills/third/SKILL.md","type":"blob","size":1,"sha":"b"}]})");

    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(tdd_ref(), {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::Malformed);
}

TEST(github_rate_limit_403_is_typed) {
    TempDir tmp("ratelimit");
    Router router;
    router.routes[kRepoUrl] = reply(403, R"({"message":"API rate limit exceeded"})",
                                    {{"x-ratelimit-remaining", "0"}, {"x-ratelimit-reset", "61"}});

    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(tdd_ref(), {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::RateLimited);
    ASSERT_EQ(r.error.retry_after_s, 60);
}

TEST(github_repo_bundle_is_cached) {
    TempDir tmp("cache");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, kTreeJson);
    router.routes[kMarketUrl] = reply(200, kMarketJson);
    router.routes[kSkillUrl] = reply(200, kSkillMd);

    const agent::GitHubIndex index(options(router, tmp.path));
    ASSERT_TRUE(index.detail(tdd_ref(), {}).ok());
    ASSERT_TRUE(index.detail(tdd_ref(), {}).ok());
    ASSERT_EQ(router.calls[kRepoUrl], 1);
    ASSERT_EQ(router.calls[kTreeUrl], 1);
    ASSERT_EQ(router.calls[kMarketUrl], 1);
}

TEST(github_search_is_unsupported) {
    TempDir tmp("search");
    Router router;
    const auto r = agent::GitHubIndex(options(router, tmp.path)).search("tdd", 20, {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::Unsupported);
}

TEST(github_truncated_tree_is_an_error) {
    TempDir tmp("truncated");
    Router router;
    router.routes[kRepoUrl] = reply(200, kRepoJson);
    router.routes[kTreeUrl] = reply(200, R"({"truncated":true,"tree":[]})");

    const auto r = agent::GitHubIndex(options(router, tmp.path)).detail(tdd_ref(), {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::Malformed);
}

TEST(github_fetch_assets_reads_each_file) {
    TempDir tmp("assets");
    Router router;
    router.routes[kSkillUrl] = reply(200, kSkillMd);
    router.routes["https://raw.githubusercontent.com/acme/skills/main/skills/tdd/tests.md"] =
        reply(200, "tests");

    agent::SkillDetail d;
    d.ref = tdd_ref();
    d.default_branch = "main";
    d.skill_dir = "skills/tdd";
    d.assets = {{"SKILL.md", 100, "aaa"}, {"tests.md", 5, "bbb"}};

    const auto r = agent::GitHubIndex(options(router, tmp.path)).fetch_assets(d, {});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(static_cast<int>(r.files->size()), 2);
    ASSERT_EQ((*r.files)[0].contents, std::string(kSkillMd));
    ASSERT_EQ((*r.files)[1].contents, std::string("tests"));
}

TEST(github_fetch_assets_stops_on_failure) {
    TempDir tmp("assets_fail");
    Router router;
    router.routes[kSkillUrl] = reply(200, kSkillMd);

    agent::SkillDetail d;
    d.ref = tdd_ref();
    d.default_branch = "main";
    d.skill_dir = "skills/tdd";
    d.assets = {{"SKILL.md", 100, "aaa"}, {"tests.md", 5, "bbb"}};

    const auto r = agent::GitHubIndex(options(router, tmp.path)).fetch_assets(d, {});
    ASSERT_TRUE(!r.ok());
}

TEST(github_fetch_assets_rejects_unsafe_path) {
    TempDir tmp("assets_unsafe");
    Router router;
    agent::SkillDetail d;
    d.ref = tdd_ref();
    d.default_branch = "main";
    d.assets = {{"../evil.md", 1, "x"}};

    const auto r = agent::GitHubIndex(options(router, tmp.path)).fetch_assets(d, {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::Malformed);
    ASSERT_EQ(static_cast<int>(router.calls.size()), 0);
}

TEST(github_fetch_assets_caps_file_count) {
    TempDir tmp("assets_cap");
    Router router;
    agent::SkillDetail d;
    d.ref = tdd_ref();
    d.default_branch = "main";
    for (int i = 0; i < 201; ++i)
        d.assets.push_back({"f" + std::to_string(i) + ".md", 1, "x"});

    const auto r = agent::GitHubIndex(options(router, tmp.path)).fetch_assets(d, {});
    ASSERT_TRUE(!r.ok());
    ASSERT_TRUE(r.error.kind == agent::IndexError::Malformed);
}
