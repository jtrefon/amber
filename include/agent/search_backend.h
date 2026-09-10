
#ifndef AGENT_SEARCH_BACKEND_H
#define AGENT_SEARCH_BACKEND_H

#include <functional>
#include <map>
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

// Registry of search backends by mode name. Backends register themselves
// at static initialization; SearchTool looks up the mode here instead of
// branching on strings (OCP: add a backend = register a factory, no edit
// to SearchTool).
class SearchBackendRegistry {
public:
    static SearchBackendRegistry& instance();
    void register_backend(const std::string& mode,
                          std::function<std::unique_ptr<SearchBackend>()> factory);
    std::unique_ptr<SearchBackend> create(const std::string& mode) const;
    std::vector<std::string> available() const;
private:
    std::map<std::string, std::function<std::unique_ptr<SearchBackend>()>> factories_;
};

} // namespace agent

#endif // AGENT_SEARCH_BACKEND_H
