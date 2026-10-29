#include "agent/well_known_index.h"

#include "agent/github_index.h"
#include "agent/remote_text.h"
#include "agent/sha256.h"
#include "agent/skill_file.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

using json = nlohmann::json;

std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Resolve an entry's URL against the index URL. Absolute URLs win; a relative
// one is joined to the index's directory, so a path-scoped index keeps its
// skills under that path.
std::string resolve_url(const std::string& base, const std::string& url) {
    if (url.empty())
        return {};
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
        return url;
    if (url.rfind('/') == 0) {
        const std::size_t scheme = base.find("://");
        const std::size_t host_end = base.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        return (host_end == std::string::npos ? base : base.substr(0, host_end)) + url;
    }
    if (base.empty() || base.back() == '/')
        return base + url;
    return base + "/" + url;
}

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool supported_schema(const std::string& schema) {
    return schema.find("v0.2") != std::string::npos || schema.find("v0.1") != std::string::npos;
}

std::optional<WellKnownFile> parse_file(const json& entry, const std::string& base) {
    if (!entry.is_object())
        return std::nullopt;
    WellKnownFile file;
    file.path = str(entry, "path");
    file.digest = str(entry, "digest");
    file.url = resolve_url(base, str(entry, "url"));
    if (file.path.empty() || file.url.empty())
        return std::nullopt;
    return file;
}

std::optional<WellKnownEntry> parse_entry(const json& entry, const std::string& base) {
    if (!entry.is_object())
        return std::nullopt;
    WellKnownEntry skill;
    skill.name = sanitize_remote_one_line(str(entry, "name"));
    if (skill.name.empty())
        return std::nullopt;
    skill.description = sanitize_remote_one_line(str(entry, "description"));
    // v0.2 carries the body as `url`; v0.1 carries it in `files`. Normalising
    // both into one file list is what lets install treat them identically.
    if (const std::string body = resolve_url(base, str(entry, "url")); !body.empty()) {
        skill.files.push_back(WellKnownFile{"SKILL.md", body, str(entry, "digest")});
    }
    if (const auto files = entry.find("files"); files != entry.end() && files->is_array()) {
        for (const auto& file : *files) {
            if (auto parsed = parse_file(file, base))
                skill.files.push_back(std::move(*parsed));
        }
    }
    const bool has_body =
        std::any_of(skill.files.begin(), skill.files.end(),
                    [](const WellKnownFile& file) { return file.path == "SKILL.md"; });
    return has_body ? std::optional<WellKnownEntry>(std::move(skill)) : std::nullopt;
}

IndexFailure transport_failure(const std::optional<HttpResponse>& response) {
    if (!response)
        return {IndexError::Offline, 0, 0, "no response"};
    if (response->status == 0)
        return {response->timed_out ? IndexError::Timeout : IndexError::Offline, 0, 0,
                response->timed_out ? "timed out" : "transport failure"};
    if (response->status == 404)
        return {IndexError::Http, 404, 0, "not found"};
    return {IndexError::Http, response->status, 0,
            "host returned HTTP " + std::to_string(response->status)};
}

} // namespace

std::vector<std::string> well_known_urls(const std::string& base_url) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.find("/.well-known/") != std::string::npos)
        return {base};
    return {base + "/.well-known/agent-skills/index.json", base + "/.well-known/skills/index.json"};
}

// Relative entry URLs are relative to the host (or the path-scoped base), not
// to the .well-known directory: an index at https://host/team/.well-known/...
// owns https://host/team/... and nothing above it.
std::string index_root(const std::string& base_url) {
    const std::size_t marker = base_url.find("/.well-known/");
    if (marker != std::string::npos)
        return base_url.substr(0, marker);
    const std::size_t slash = base_url.find('/', base_url.find("://") + 3);
    return slash == std::string::npos ? base_url : base_url.substr(0, slash);
}

// The schema gate: a version amber does not know is refused by name, and an
// index with no $schema is the legacy (v0.1) shape by definition.
bool read_schema(const json& j, WellKnownTable& table, std::string& error) {
    const auto schema = j.find("$schema");
    if (schema == j.end()) {
        table.legacy = true;
        return true;
    }
    if (!schema->is_string()) {
        error = "index $schema is not a string";
        return false;
    }
    table.schema = schema->get<std::string>();
    if (!supported_schema(table.schema)) {
        error = "unsupported $schema: " + table.schema;
        return false;
    }
    table.legacy = table.schema.find("v0.1") != std::string::npos;
    return true;
}

WellKnownTable parse_well_known_index(const std::string& body, const std::string& base_url,
                                      std::string& error) {
    WellKnownTable table;
    error.clear();
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "index is not a JSON object";
        return table;
    }
    if (!read_schema(j, table, error))
        return table;
    // Relative entry URLs are relative to the host (or the path-scoped base),
    // not to the .well-known directory: an index at
    // https://host/team/.well-known/... owns https://host/team/... and nothing
    // above it.
    const std::string root = index_root(base_url);
    const auto skills = j.find("skills");
    if (skills == j.end() || !skills->is_array()) {
        error = "index has no skills array";
        return table;
    }
    for (const auto& entry : *skills) {
        if (auto parsed = parse_entry(entry, root))
            table.skills.push_back(std::move(*parsed));
    }
    if (table.skills.empty()) {
        error = "index lists no usable skill";
        return table;
    }
    return table;
}

bool digest_matches(const std::string& bytes, const std::string& digest) {
    if (digest.empty())
        return true; // nothing to verify, and the absence is recorded by the caller
    std::string hex = digest;
    if (hex.rfind("sha256:", 0) == 0)
        hex = hex.substr(7);
    if (hex.size() != 64)
        return false;
    if (!std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch); }))
        return false;
    return sha256_hex(bytes) == to_lower(hex);
}

WellKnownIndex::WellKnownIndex(WellKnownOptions opts) : opts_(std::move(opts)) {}

IndexResult WellKnownIndex::search(const std::string&, int, const CancellationToken&) const {
    return {std::nullopt, {IndexError::Unsupported, 0, 0, "well-known sources do not rank"}};
}

std::vector<SourceStatus> WellKnownIndex::sources() const {
    SourceStatus status;
    status.name = opts_.base_url;
    status.enabled = !opts_.base_url.empty();
    status.fetched_at_ms = opts_.now_ms ? opts_.now_ms() : 0;
    return {status};
}

namespace {

// Fetch the index, preferred URL first. A 404 moves on; any other failure is
// reported rather than hidden behind the legacy location.
std::optional<WellKnownTable> load_table(const WellKnownOptions& opts,
                                         const CancellationToken& cancel, std::string& error) {
    for (const auto& url : well_known_urls(opts.base_url)) {
        const HttpGet get = opts.http ? opts.http : HttpGet(http_get);
        const auto response = get(url, "", opts.timeout_s, cancel);
        if (!response)
            continue;
        if (response->status == 404)
            continue;
        if (response->status < 200 || response->status >= 300) {
            error = transport_failure(response).detail;
            return std::nullopt;
        }
        WellKnownTable table = parse_well_known_index(response->body, url, error);
        if (!error.empty())
            return std::nullopt;
        return table;
    }
    error = "no well-known index at " + opts.base_url;
    return std::nullopt;
}

const WellKnownEntry* find_entry(const WellKnownTable& table, const SkillRef& ref) {
    const std::string slug = skill_slug(ref.id);
    for (const auto& entry : table.skills) {
        if (entry.name == slug || entry.name == ref.name)
            return &entry;
    }
    return nullptr;
}

// One file's bytes with its digest checked. A mismatch is the failure #409
// cares about: nothing may be written from it.
bool fetch_verified(const WellKnownOptions& opts, const WellKnownFile& file,
                    const CancellationToken& cancel, std::string& contents, std::string& error) {
    const HttpGet get = opts.http ? opts.http : HttpGet(http_get);
    const auto response = get(file.url, "", opts.timeout_s, cancel);
    if (!response || response->status < 200 || response->status >= 300) {
        error = file.path + ": " + transport_failure(response).detail;
        return false;
    }
    if (!digest_matches(response->body, file.digest)) {
        error = file.path + ": digest mismatch (nothing was written)";
        return false;
    }
    contents = response->body;
    return true;
}

} // namespace

DetailResult WellKnownIndex::detail(const SkillRef& ref, const CancellationToken& cancel) const {
    std::string error;
    const auto table = load_table(opts_, cancel, error);
    if (!table)
        return {std::nullopt, {IndexError::Malformed, 0, 0, error}};
    const WellKnownEntry* entry = find_entry(*table, ref);
    if (!entry)
        return {std::nullopt, {IndexError::Http, 404, 0, "host does not list this skill"}};

    std::string body;
    const WellKnownFile* skill_md = nullptr;
    for (const auto& file : entry->files) {
        if (file.path == "SKILL.md")
            skill_md = &file;
    }
    if (!skill_md || !fetch_verified(opts_, *skill_md, cancel, body, error))
        return {std::nullopt, {IndexError::Malformed, 0, 0, error}};
    const auto meta = parse_skill_meta(body);
    if (!meta)
        return {std::nullopt, {IndexError::Malformed, 0, 0, "SKILL.md frontmatter did not parse"}};

    SkillDetail detail;
    detail.ref = ref;
    detail.ref.name = entry->name;
    detail.body = sanitize_remote_text(body);
    detail.description = entry->description.empty() ? sanitize_remote_one_line(meta->description)
                                                    : entry->description;
    detail.license = sanitize_remote_one_line(meta->license);
    detail.hash = skill_md->digest;
    for (const auto& file : entry->files)
        detail.assets.push_back(
            {file.path, file.path == "SKILL.md" ? body.size() : 0, file.digest});
    return {std::move(detail), {}};
}

AssetsResult WellKnownIndex::fetch_assets(const SkillDetail& detail,
                                          const CancellationToken& cancel) const {
    std::string error;
    const auto table = load_table(opts_, cancel, error);
    if (!table)
        return {std::nullopt, {IndexError::Malformed, 0, 0, error}};
    const WellKnownEntry* entry = find_entry(*table, detail.ref);
    if (!entry)
        return {std::nullopt, {IndexError::Http, 404, 0, "host does not list this skill"}};

    std::vector<SkillAssetContent> files;
    for (const auto& file : entry->files) {
        std::string contents;
        if (!fetch_verified(opts_, file, cancel, contents, error))
            return {std::nullopt, {IndexError::Malformed, 0, 0, error}};
        files.push_back({file.path, std::move(contents)});
    }
    return {std::move(files), {}};
}

} // namespace agent
