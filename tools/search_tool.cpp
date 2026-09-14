
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/search_backend.h"
#include "agent/workspace.h"
#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace agent {

// search: dispatches to the backend the `mode` argument selects. Backends are
// plugin contributions; the tool resolves them through the provider it was
// built with (see search_backend.h), so this file names no backend, and adding
// one is a plugin, never an edit here.
// Args:
//   pattern (string, required) query / regex pattern
//   path    (string, optional)  directory or file (default: workspace root)
//   glob    (string, optional)  restrict to a glob, e.g. "*.cpp"
//   mode    (string, optional)  enabled backend name (default: "grep")
//   max     (int, optional)     max matches to return (default 200)
class SearchTool : public Tool {
public:
    explicit SearchTool(SearchBackendProvider provider) : provider_(std::move(provider)) {}

    std::string name() const noexcept override { return "search"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Search the codebase. The `mode` argument selects an enabled "
               "search backend (\"grep\" is the default). Useful for finding "
               "where a symbol is referenced before reading the file. Patterns "
               "longer than 256 characters are rejected; results are capped by "
               "`max`.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"pattern",
               {{"type", "string"},
                {"description", "Regular expression or query (max 256 chars)"},
                {"maxLength", 256}}},
              {"path",
               {{"type", "string"},
                {"description", "Directory or file (default workspace root). "
                                "Hidden dirs and vendored code are skipped "
                                "by default; set a path inside one to "
                                "search it explicitly."}}},
              {"glob", {{"type", "string"}, {"description", "Optional glob filter, e.g. '*.cpp'"}}},
              {"mode", {{"type", "string"}, {"description", mode_description()}}},
              {"max", {{"type", "integer"}, {"description", "Max matches (default 200)"}}}}},
            {"required", {"pattern"}}};
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("pattern") || !a["pattern"].is_string()) {
            r.ok = false;
            r.error = "missing 'pattern'";
            return r;
        }
        std::string pattern = a["pattern"].get<std::string>();
        if (pattern.size() > 256) {
            r.ok = false;
            r.error = "pattern too long (" + std::to_string(pattern.size()) +
                      " chars); keep it under 256 and use a specific token, not "
                      "a giant alternation of every symbol";
            return r;
        }
        // Confine the search root to the workspace: an absolute or
        // out-of-workspace path is refused (search is read-only and ungated,
        // so honoring an escape would hand the model arbitrary file
        // contents). A bare search defaults to the workspace root so it
        // covers the project regardless of the process cwd.
        std::string req_path = a.value("path", std::string(""));
        std::string path = agent::Workspace::root();
        if (!req_path.empty()) {
            std::string confined, err;
            if (!agent::Workspace::confine(req_path, confined, err)) {
                r.ok = false;
                r.error = err;
                return r;
            }
            path = confined;
        }
        // Hidden/vendored dirs are skipped by default; an explicit path
        // inside one of them means the agent deliberately wants it, so that
        // exclusion is dropped.
        std::vector<std::string> excludes = agent::default_excluded_dirs();
        if (!req_path.empty()) {
            std::string rel = agent::Workspace::relative(path);
            std::string first = rel.substr(0, rel.find('/'));
            excludes.erase(std::remove_if(excludes.begin(), excludes.end(),
                                          [&](const std::string& d) { return d == first; }),
                           excludes.end());
        }
        std::string glob = a.value("glob", std::string(""));
        std::string mode = a.value("mode", std::string("grep"));
        long max = a.value("max", 200L);
        if (max < 1)
            max = 1;

        // A mode that resolves to nothing fails loudly: a disabled backend
        // names the plugin and the command that re-enables it, an unknown one
        // lists what is enabled. There is no silent fallback to a built-in.
        SearchBackendLookup lookup = provider_.resolve(mode);
        if (!lookup.backend) {
            r.ok = false;
            r.error = lookup.error.empty() ? ("unknown search mode: " + mode) : lookup.error;
            return r;
        }
        const std::string backend_name = lookup.backend->name();
        std::vector<SearchHit> hits = lookup.backend->search(pattern, path, glob, max, excludes);

        std::stringstream out;
        if (hits.empty()) {
            out << "no matches (" << backend_name << ")";
        } else {
            out << "[" << backend_name << "] " << hits.size() << " hit(s):\n";
            for (const auto& h : hits) {
                std::string rel = Workspace::relative(h.path);
                if (backend_name == "semantic")
                    out << rel << ":" << h.line_no << " (score=" << h.score << ") " << h.line
                        << "\n";
                else
                    out << rel << ":" << h.line_no << ":" << h.line << "\n";
            }
        }
        r.output = out.str();
        if (!r.output.empty() && r.output.back() == '\n')
            r.output.pop_back();
        r.meta = {{"hits", static_cast<long>(hits.size())}, {"mode", mode}};
        return r;
    }

private:
    // Rebuilt with the schema on every request, so switching a backend plugin
    // off updates the model-facing documentation in the same action: the
    // schema and the registry cannot disagree about what exists.
    std::string mode_description() const {
        std::vector<std::string> modes;
        if (provider_.modes)
            modes = provider_.modes();
        if (modes.empty())
            return "No search backend is currently enabled.";
        std::string list;
        for (const auto& m : modes)
            list += (list.empty() ? "" : ", ") + m;
        return "Backend to use; one of: " + list + " (default \"grep\").";
    }

    SearchBackendProvider provider_;
};

std::unique_ptr<Tool> make_search_tool(SearchBackendProvider provider) {
    return std::make_unique<SearchTool>(std::move(provider));
}

} // namespace agent
