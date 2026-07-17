// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/search_backend.h"
#include <memory>
#include <sstream>
#include <string>

using agent::make_grep_backend;
using agent::make_semantic_backend;

namespace agent {

// search: dispatches to a pluggable backend. Defaults to a grep wrapper; the
// `mode` argument selects "semantic" (local index + cosine ranking) so the
// schema the model sees stays stable as backends evolve.
// Args:
//   pattern (string, required) query / regex pattern
//   path    (string, optional)  directory or file (default: ".")
//   glob    (string, optional)  restrict to a glob, e.g. "*.cpp"
//   mode    (string, optional)  "grep" (default) or "semantic"
//   max     (int, optional)     max matches to return (default 200)
class SearchTool : public Tool {
public:
    std::string name() const override { return "search"; }

    std::string description() const override {
        return "Search the codebase. The default mode runs a regex search "
               "(grep). Set mode=\"semantic\" for meaning-based ranking over an "
               "indexed view of the tree (useful for paraphrased queries). "
               "Prefer search over reading whole trees to locate symbols.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"},
                             {"description", "Regular expression or query to search for"}}},
                {"path", {{"type", "string"},
                          {"description", "Directory or file (default '.')"}}},
                {"glob", {{"type", "string"},
                          {"description", "Optional glob filter, e.g. '*.cpp'"}}},
                {"mode", {{"type", "string"},
                          {"description", "'grep' (default) or 'semantic'"}}},
                {"max", {{"type", "integer"},
                         {"description", "Max matches (default 200)"}}}
            }},
            {"required", {"pattern"}}
        };
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("pattern") || !a["pattern"].is_string()) {
            r.ok = false; r.error = "missing 'pattern'"; return r;
        }
        std::string pattern = a["pattern"].get<std::string>();
        std::string path = a.value("path", std::string("."));
        std::string glob = a.value("glob", std::string(""));
        std::string mode = a.value("mode", std::string("grep"));
        long max = a.value("max", 200L);
        if (max < 1) max = 1;

        std::unique_ptr<SearchBackend> backend;
        if (mode == "semantic") backend = make_semantic_backend();
        else backend = make_grep_backend();

        auto hits = backend->search(pattern, path, glob, max);

        std::stringstream out;
        if (hits.empty()) {
            out << "no matches (" << backend->name() << ")";
        } else {
            out << "[" << backend->name() << "] " << hits.size() << " hit(s):\n";
            for (const auto& h : hits) {
                if (mode == "semantic")
                    out << h.path << ":" << h.line_no << " (score=" << h.score
                        << ") " << h.line << "\n";
                else
                    out << h.path << ":" << h.line_no << ":" << h.line << "\n";
            }
        }
        r.output = out.str();
        if (!r.output.empty() && r.output.back() == '\n') r.output.pop_back();
        return r;
    }
};

std::unique_ptr<Tool> make_search_tool() {
    return std::make_unique<SearchTool>();
}

} // namespace agent
