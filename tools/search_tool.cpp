
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/search_backend.h"
#include "agent/workspace.h"
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
    std::string name() const noexcept override { return "search"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Search the codebase. Locates matches by regex "
                "(mode=\"grep\", default) or meaning-based ranking "
                "(mode=\"semantic\"). Useful for finding where a symbol is "
                "referenced before reading the file. Patterns longer than 256 "
                "characters are rejected; results are capped by `max`.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"},
                             {"description", "Regular expression or query (max 256 chars)"},
                             {"maxLength", 256}}},
                {"path", {{"type", "string"},
                          {"description", "Directory or file (default workspace root). "
                                          "Hidden dirs and vendored code are skipped "
                                          "by default; set a path inside one to "
                                          "search it explicitly."}}},
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
        if (pattern.size() > 256) {
            r.ok = false;
            r.error = "pattern too long (" + std::to_string(pattern.size()) +
                      " chars); keep it under 256 and use a specific token, not "
                      "a giant alternation of every symbol";
            return r;
        }
        // Default the search root to the workspace root so a bare search always
        // covers the project regardless of the process cwd (the other
        // filesystem tools already confine to the workspace). An explicit
        // relative path is confined under the workspace; an absolute or
        // out-of-workspace path is honoured as given (search is read-only).
        std::string req_path = a.value("path", std::string(""));
        std::string path = agent::Workspace::root();
        if (!req_path.empty()) {
            std::string confined, err;
            path = (agent::Workspace::confine(req_path, confined, err))
                       ? confined
                       : req_path;
        }
        // Hidden/vendored dirs are skipped by default; an explicit path
        // inside one of them means the agent deliberately wants it, so that
        // exclusion is dropped.
        std::vector<std::string> excludes = agent::default_excluded_dirs();
        if (!req_path.empty()) {
            std::string rel = agent::Workspace::relative(path);
            std::string first = rel.substr(0, rel.find('/'));
            excludes.erase(
                std::remove_if(excludes.begin(), excludes.end(),
                               [&](const std::string& d) {
                                   return d == first;
                               }),
                excludes.end());
        }
        std::string glob = a.value("glob", std::string(""));
        std::string mode = a.value("mode", std::string("grep"));
        long max = a.value("max", 200L);
        if (max < 1) max = 1;

        std::unique_ptr<SearchBackend> backend;
        if (mode == "semantic") backend = make_semantic_backend();
        else backend = make_grep_backend();

        auto hits = backend->search(pattern, path, glob, max, excludes);

        std::stringstream out;
        if (hits.empty()) {
            out << "no matches (" << backend->name() << ")";
        } else {
            out << "[" << backend->name() << "] " << hits.size() << " hit(s):\n";
            for (const auto& h : hits) {
                std::string rel = Workspace::relative(h.path);
                if (mode == "semantic")
                    out << rel << ":" << h.line_no << " (score=" << h.score
                        << ") " << h.line << "\n";
                else
                    out << rel << ":" << h.line_no << ":" << h.line << "\n";
            }
        }
        r.output = out.str();
        if (!r.output.empty() && r.output.back() == '\n') r.output.pop_back();
        r.meta = {{"hits", static_cast<long>(hits.size())}, {"mode", mode}};
        return r;
    }
};

std::unique_ptr<Tool> make_search_tool() {
    return std::make_unique<SearchTool>();
}

} // namespace agent
