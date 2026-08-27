
#include "agent/search_backend.h"
#include "agent/semantic_helpers.h"
#include <array>
#include <cstdio>
#include <memory>
#include <sstream>

namespace agent {

// grep-backed search. Wraps `grep -rnI` with shell-safe quoting. This is the
// default backend and preserves the exact behavior the tool had before the
// backend abstraction was introduced.
class GrepBackend : public SearchBackend {
public:
    std::string name() const noexcept override { return "grep"; }

    std::vector<SearchHit> search(
        const std::string& query, const std::string& root,
        const std::string& glob, long max,
        const std::vector<std::string>& exclude_dirs =
            default_excluded_dirs()) const override {
        std::vector<SearchHit> hits;
        // Exclude hidden metadata dirs and vendored code by default; searching
        // them returns escaped JSON blobs that inflate the conversation past
        // any server's max payload (~440MB in practice), triggering HTTP 413.
        // The tool may pass a reduced list when the agent explicitly targets
        // one of these dirs.
        std::string cmd =
            "grep -rnIE --line-number --max-count=10000 ";
        for (const auto& d : exclude_dirs) {
            cmd += "--exclude-dir=";
            cmd += shell_quote(d);
            cmd += " ";
        }
        if (!glob.empty()) {
            cmd += "--include=";
            cmd += shell_quote(glob);
            cmd += " ";
        }
        cmd += shell_quote(query) + " " + shell_quote(root) +
               " 2>/dev/null | head -n " + std::to_string(max);

        std::string out = pipe_read(cmd);
        std::stringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            // format: path:lineno:text
            size_t c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            size_t c2 = line.find(':', c1 + 1);
            SearchHit h;
            h.path = line.substr(0, c1);
            if (c2 != std::string::npos) {
                try { h.line_no = std::stol(line.substr(c1 + 1, c2 - c1 - 1)); }
                catch (...) { h.line_no = 0; }
                h.line = line.substr(c2 + 1);
            } else {
                h.line = line.substr(c1 + 1);
            }
            h.score = static_cast<double>(hits.size());  // preserve order
            hits.push_back(std::move(h));
        }
        return hits;
    }

private:
    static std::string pipe_read(const std::string& cmd) {
        std::string result;
        std::array<char, 256> buf{};
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return result;
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
            result += buf.data();
        pclose(pipe);
        if (!result.empty() && result.back() == '\n') result.pop_back();
        return result;
    }
};

std::unique_ptr<SearchBackend> make_grep_backend() {
    return std::make_unique<GrepBackend>();
}

} // namespace agent
