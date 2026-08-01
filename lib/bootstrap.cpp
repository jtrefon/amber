
#include "agent/bootstrap.h"

#include "agent/config.h"
#include "agent/data_path.h"

#include <cstdio>

namespace agent {

namespace {

// A file is "missing" when it resolves nowhere in the candidate search.
// The message names the file and every location searched, so a packaged
// install failure is diagnosable from the stderr output alone.
std::string describe_missing(const std::string& label,
                             const std::string& path, const char* argv0) {
    auto candidates = data_file_candidates(path, argv0);
    std::string msg = label + ": " + path + " (searched:";
    for (const auto& c : candidates) msg += " " + c;
    msg += ")";
    return msg;
}

} // namespace

std::vector<std::string> missing_bootstrap_files(const Config& cfg,
                                                 const char* argv0,
                                                 bool require_completions) {
    std::vector<std::string> out;
    if (!cfg.system_prompt_path.empty() &&
        resolve_data_path(cfg.system_prompt_path, argv0).empty())
        out.push_back(describe_missing("system prompt", cfg.system_prompt_path,
                                       argv0));
    // An explicitly cleared tools prompt means the auto-rendered reference is
    // intended; only a configured-but-unfindable file is fatal.
    if (!cfg.tools_prompt_path.empty() &&
        resolve_data_path(cfg.tools_prompt_path, argv0).empty())
        out.push_back(
            describe_missing("tools prompt", cfg.tools_prompt_path, argv0));
    if (require_completions &&
        resolve_data_path("completions.json", argv0).empty())
        out.push_back(describe_missing("command tree", "completions.json",
                                       argv0));
    return out;
}

} // namespace agent
