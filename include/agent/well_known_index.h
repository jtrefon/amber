#ifndef AMBER_AGENT_WELL_KNOWN_INDEX_H
#define AMBER_AGENT_WELL_KNOWN_INDEX_H

#include <functional>
#include <string>
#include <vector>

#include "agent/skill_index.h"

namespace agent {

// One skill in a host's well-known index. Two shapes are in the wild and both
// are accepted:
//
//   v0.2  {"name": "tdd", "url": "tdd/SKILL.md", "digest": "sha256:…",
//          "files": [{"path": "notes.md", "url": "tdd/notes.md", ...}]}
//   v0.1  {"name": "tdd", "files": [{"path": "SKILL.md", "url": "…", ...}]}
//
// Every file carries its own digest, so verification is per file rather than
// per pack.
struct WellKnownFile {
    std::string path; // relative to the skill directory
    std::string url;  // absolute, after resolution against the index URL
    std::string digest;
};

struct WellKnownEntry {
    std::string name;
    std::string description;
    std::vector<WellKnownFile> files;
};

struct WellKnownTable {
    std::string schema;
    bool legacy = false; // the v0.1 files-only model
    std::vector<WellKnownEntry> skills;
};

// Parse an index. Unknown `$schema` values are an error with the value named,
// and an index that lists no usable skill is an error too: a path-scoped URL
// that has nothing under it must not fall back to installing a whole host.
WellKnownTable parse_well_known_index(const std::string& body, const std::string& base_url,
                                      std::string& error);

// The two locations, preferred first, for a host or path-scoped base URL.
std::vector<std::string> well_known_urls(const std::string& base_url);

// `sha256:<hex>` or a bare 64-character hex digest. Anything else is a mismatch:
// an integrity check that cannot be performed must not pass.
bool digest_matches(const std::string& bytes, const std::string& digest);

struct WellKnownOptions {
    std::string base_url; // "https://host" or a path-scoped base
    long timeout_s = 15;
    HttpGet http;
    std::function<long long()> now_ms;
};

// Per-host adapter: it does not rank, so `search` answers Unsupported and a
// source appears in search only when some other adapter returns it.
class WellKnownIndex final : public SkillIndex {
public:
    explicit WellKnownIndex(WellKnownOptions opts);

    IndexResult search(const std::string& query, int limit,
                       const CancellationToken& cancel) const override;
    DetailResult detail(const SkillRef& ref, const CancellationToken& cancel) const override;
    AssetsResult fetch_assets(const SkillDetail& detail,
                              const CancellationToken& cancel) const override;
    std::vector<SourceStatus> sources() const override;

private:
    WellKnownOptions opts_;
};

} // namespace agent

#endif // AMBER_AGENT_WELL_KNOWN_INDEX_H
