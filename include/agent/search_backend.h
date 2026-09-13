
#ifndef AGENT_SEARCH_BACKEND_H
#define AGENT_SEARCH_BACKEND_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

// Directory names skipped by default when searching the workspace root:
// hidden metadata dirs and vendored code. The SearchTool drops an entry when
// the agent explicitly targets a path inside one of them — hidden/vendored
// content stays searchable on purpose, the default just keeps scans clean.
inline const std::vector<std::string>& default_excluded_dirs() {
    static const std::vector<std::string> dirs = {".amber", ".git",
                                                  "third_party"};
    return dirs;
}

// A single search hit. `line` is the matched/ranked line (or snippet); `score`
// is backend-specific (grep uses match order, semantic uses cosine distance).
struct SearchHit {
    std::string path;
    long line_no = 0;
    std::string line;
    double score = 0.0;
};

// Pluggable search strategy. The `SearchTool` owns one backend and delegates to
// it. New backends (indexed, semantic, remote) implement this interface and
// are selected via the tool's `mode` argument without changing the schema.
class SearchBackend {
public:
    virtual ~SearchBackend() = default;

    // `query` is the user/model pattern. `root` is the path to search. `glob`
    // optionally restricts files. `max` caps the number of returned hits.
    // `exclude_dirs` lists directory names to skip (default: the standard
    // hidden/vendored set); pass an empty vector to search everything.
    virtual std::vector<SearchHit> search(
        const std::string& query, const std::string& root,
        const std::string& glob, long max,
        const std::vector<std::string>& exclude_dirs =
            default_excluded_dirs()) const = 0;

    virtual std::string name() const noexcept = 0;
};

// Backend factories (defined in tools/search/{grep,semantic}_backend.cpp).
std::unique_ptr<SearchBackend> make_grep_backend();
std::unique_ptr<SearchBackend> make_semantic_backend();

// Resolving a mode: a backend, or the reason there is none. `error` is
// fully-formed and user-facing - it names the plugin to enable when the mode is
// provided by a switched-off plugin. The search tool relays it verbatim.
struct SearchBackendLookup {
    std::unique_ptr<SearchBackend> backend;
    std::string error; // set when backend == nullptr
};

// What the search tool resolves modes through. The tool holds it by value and
// the callables keep whatever provides them alive (the runtime's registry in a
// hosted session, a private one in a bare host), so a tool can never outlive
// the backends it resolves.
struct SearchBackendProvider {
    // Enabled mode names, in registration order. The tool renders these into
    // the `mode` argument's description, so schema and registry cannot disagree
    // about what exists.
    std::function<std::vector<std::string>()> modes;
    std::function<SearchBackendLookup(const std::string& mode)> resolve;
};

// The owner-tagged table itself is a plugin contribution registry; it is
// declared with the others in extensions.h.
class SearchBackendRegistry;

// Build a provider over a registry (defined in lib/extensions.cpp). The
// returned callables copy the pointer, so whatever holds the provider keeps the
// table alive.
SearchBackendProvider
make_search_backend_provider(const std::shared_ptr<SearchBackendRegistry>& registry);

// The backends amber ships, without a runtime: the same capabilities the
// bundled backend plugins declare, installed into a private registry the
// returned provider keeps alive. For hosts and tests that hold a bare tool
// registry and no PluginRuntime.
SearchBackendProvider builtin_search_backend_provider();

} // namespace agent

#endif // AGENT_SEARCH_BACKEND_H
