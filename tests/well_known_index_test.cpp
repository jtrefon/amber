#include "agent/sha256.h"
#include "agent/well_known_index.h"
#include "tests/test_util.h"

#include <map>
#include <optional>
#include <string>

namespace {

agent::HttpResponse reply(long status, std::string body) {
    agent::HttpResponse r;
    r.status = status;
    r.body = std::move(body);
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

agent::WellKnownOptions options(Router& router) {
    agent::WellKnownOptions o;
    o.base_url = "https://skills.example.com";
    o.http = [&router](const std::string& url, const std::string&, long,
                       const agent::CancellationToken&) { return router.get(url); };
    return o;
}

const std::string kIndexUrl = "https://skills.example.com/.well-known/agent-skills/index.json";
const std::string kLegacyUrl = "https://skills.example.com/.well-known/skills/index.json";
const std::string kBodyUrl = "https://skills.example.com/skills/tdd/SKILL.md";
const std::string kNotesUrl = "https://skills.example.com/skills/tdd/notes.md";

const char* kBody = "---\nname: tdd\ndescription: Red first.\nlicense: MIT\n---\nbody\n";
const char* kNotes = "notes\n";

std::string digest_of(const std::string& text) {
    return "sha256:" + agent::sha256_hex(text);
}

std::string v02_index() {
    return std::string(R"({"$schema":"https://agentskills.io/schemas/index/v0.2","skills":[)") +
           R"({"name":"tdd","description":"Red first.","url":"skills/tdd/SKILL.md","digest":")" +
           digest_of(kBody) + R"(","files":[{"path":"notes.md","url":"skills/tdd/notes.md",)" +
           R"("digest":")" + digest_of(kNotes) + R"("}]}]})";
}

std::string v01_index() {
    return std::string(R"({"skills":[{"name":"tdd","files":[)") +
           R"({"path":"SKILL.md","url":"skills/tdd/SKILL.md","digest":")" + digest_of(kBody) +
           R"("},{"path":"notes.md","url":"skills/tdd/notes.md","digest":")" + digest_of(kNotes) +
           R"("}]}]})";
}

} // namespace

TEST(sha256_matches_published_vectors) {
    ASSERT_EQ(agent::sha256_hex(""),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ASSERT_EQ(agent::sha256_hex("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ASSERT_EQ(agent::sha256_hex("The quick brown fox jumps over the lazy dog"),
              std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

TEST(well_known_v02_parses_and_resolves_relative_urls) {
    std::string error;
    const auto table = agent::parse_well_known_index(
        v02_index(), "https://skills.example.com/.well-known/agent-skills/index.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(table.skills.size(), static_cast<size_t>(1));
    ASSERT_EQ(table.skills[0].name, std::string("tdd"));
    ASSERT_EQ(table.skills[0].files.size(), static_cast<size_t>(2));
    ASSERT_EQ(table.skills[0].files[0].path, std::string("SKILL.md"));
    ASSERT_EQ(table.skills[0].files[0].url, kBodyUrl);
    ASSERT_EQ(table.skills[0].files[1].path, std::string("notes.md"));
    ASSERT_FALSE(table.legacy);
}

TEST(well_known_v01_files_model_parses) {
    std::string error;
    const auto table = agent::parse_well_known_index(
        v01_index(), "https://skills.example.com/.well-known/skills/index.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(table.skills.size(), static_cast<size_t>(1));
    ASSERT_TRUE(table.legacy);
    ASSERT_EQ(table.skills[0].files.size(), static_cast<size_t>(2));
    ASSERT_EQ(table.skills[0].files[0].url, kBodyUrl);
}

TEST(well_known_unknown_schema_is_rejected_with_the_value_named) {
    std::string error;
    const auto table = agent::parse_well_known_index(
        R"({"$schema":"https://example.com/index/v9.9","skills":[{"name":"x"}]})",
        "https://skills.example.com", error);
    ASSERT_TRUE(table.skills.empty());
    ASSERT_TRUE(error.find("unsupported $schema") != std::string::npos);
    ASSERT_TRUE(error.find("v9.9") != std::string::npos);
}

TEST(well_known_path_scoped_index_with_no_skills_is_an_error) {
    std::string error;
    const auto table = agent::parse_well_known_index(
        R"({"skills":[]})", "https://skills.example.com/team/.well-known/agent-skills/index.json",
        error);
    ASSERT_TRUE(table.skills.empty());
    ASSERT_TRUE(error.find("no usable skill") != std::string::npos);

    // Relative urls under a path-scoped index stay under that path.
    const auto scoped = agent::parse_well_known_index(
        v02_index(), "https://skills.example.com/team/.well-known/agent-skills/index.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(scoped.skills[0].files[0].url,
              std::string("https://skills.example.com/team/skills/tdd/SKILL.md"));
}

TEST(well_known_digest_mismatch_writes_nothing) {
    Router router;
    router.routes[kIndexUrl] = reply(200, v02_index());
    std::string tampered = kBody;
    tampered += "tampered";
    router.routes[kBodyUrl] = reply(200, tampered);
    router.routes[kNotesUrl] = reply(200, kNotes);

    agent::SkillRef ref;
    ref.id = "skills.example.com/tdd";
    ref.name = "tdd";
    ref.source = "skills.example.com";
    const agent::WellKnownIndex index(options(router));
    const auto detail = index.detail(ref, {});
    ASSERT_TRUE(!detail.detail);
    ASSERT_TRUE(detail.error.detail.find("digest mismatch") != std::string::npos);
    ASSERT_TRUE(detail.error.detail.find("nothing was written") != std::string::npos);
}

TEST(well_known_detail_verifies_and_fetch_assets_returns_every_file) {
    Router router;
    router.routes[kIndexUrl] = reply(200, v02_index());
    router.routes[kBodyUrl] = reply(200, kBody);
    router.routes[kNotesUrl] = reply(200, kNotes);

    agent::SkillRef ref;
    ref.id = "skills.example.com/tdd";
    ref.name = "tdd";
    ref.source = "skills.example.com";
    const agent::WellKnownIndex index(options(router));
    const auto detail = index.detail(ref, {});
    ASSERT_TRUE(detail.ok());
    ASSERT_EQ(detail.detail->ref.name, std::string("tdd"));
    ASSERT_EQ(detail.detail->description, std::string("Red first."));
    ASSERT_EQ(detail.detail->license, std::string("MIT"));
    ASSERT_EQ(detail.detail->assets.size(), static_cast<size_t>(2));

    const auto assets = index.fetch_assets(*detail.detail, {});
    ASSERT_TRUE(assets.ok());
    ASSERT_EQ(assets.files->size(), static_cast<size_t>(2));
    ASSERT_EQ((*assets.files)[0].path, std::string("SKILL.md"));
    ASSERT_EQ((*assets.files)[1].contents, std::string(kNotes));
}

TEST(well_known_falls_back_to_the_legacy_index_url) {
    Router router;
    router.routes[kIndexUrl] = reply(404, "");
    router.routes[kLegacyUrl] = reply(200, v01_index());
    router.routes[kBodyUrl] = reply(200, kBody);
    router.routes[kNotesUrl] = reply(200, kNotes);

    agent::SkillRef ref;
    ref.id = "skills.example.com/tdd";
    ref.name = "tdd";
    ref.source = "skills.example.com";
    const auto detail = agent::WellKnownIndex(options(router)).detail(ref, {});
    ASSERT_TRUE(detail.ok());
    ASSERT_EQ(router.calls[kLegacyUrl], 1);
}

TEST(well_known_does_not_rank_and_still_reports_its_source) {
    Router router;
    const agent::WellKnownIndex index(options(router));
    const auto found = index.search("tdd", 20, {});
    ASSERT_TRUE(found.error.kind == agent::IndexError::Unsupported);
    ASSERT_TRUE(!found.page);
    const auto sources = index.sources();
    ASSERT_EQ(sources.size(), static_cast<size_t>(1));
    ASSERT_TRUE(sources[0].enabled);
}
