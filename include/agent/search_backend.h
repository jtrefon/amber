// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_SEARCH_BACKEND_H
#define AGENT_SEARCH_BACKEND_H

#include <string>
#include <vector>
#include <memory>

namespace agent {

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
    virtual std::vector<SearchHit> search(const std::string& query,
                                          const std::string& root,
                                          const std::string& glob,
                                          long max) const = 0;

    virtual std::string name() const noexcept = 0;
};

// Backend factories (defined in tools/search/{grep,semantic}_backend.cpp).
std::unique_ptr<SearchBackend> make_grep_backend();
std::unique_ptr<SearchBackend> make_semantic_backend();

} // namespace agent

#endif // AGENT_SEARCH_BACKEND_H
