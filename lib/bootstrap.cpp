
#include "agent/bootstrap.h"

#include "agent/config.h"
#include "agent/data_path.h"

#include <cstdio>
#include <filesystem>
#include <sstream>

namespace agent {

namespace {

// A file is "missing" when it resolves nowhere in the candidate search. The
// message names the file, every location searched — annotated with whether
// the directory/file exists there — the install prefix the app believes it
// lives in, and a hint, so a packaged install failure is diagnosable from
// the stderr output alone instead of leaving the user guessing where the
// data bundle went.
std::string describe_missing(const std::string& label,
                             const std::string& path, const char* argv0) {
    auto candidates = data_file_candidates(path, argv0);
    std::ostringstream msg;
    msg << label << ": " << path << "\n";
    msg << "    searched:";
    if (candidates.empty()) msg << " (none)\n";
    for (const auto& c : candidates) {
        std::error_code ec;
        const bool is_dir = std::filesystem::is_directory(c, ec);
        const bool exists = file_exists(c);
        msg << "\n      " << c;
        if (exists) msg << "  [found]";
        else if (is_dir) msg << "  [directory exists, file missing]";
        else msg << "  [missing]";
    }
    // Where the app believes its install prefix is, and where it expects the
    // data bundle to live — the single most useful hint for a bad install.
    std::string exed = exe_dir();
    if (!exed.empty()) {
        std::string expected = exed + "/../share/amber/" + path;
        std::error_code ec2;
        msg << "\n    expected: " << expected
            << (file_exists(expected) ? "  [found]" : "  [missing]");
        msg << "\n    hint: install the amber data bundle (prompts/ + "
               "completions.json)";
        msg << "\n          next to the binary (" << exed
            << ") or under one of the paths above.";
    } else {
        msg << "\n    hint: run amber from its install prefix, e.g. "
               "`/opt/homebrew/bin/amber`.";
    }
    return msg.str();
}

} // namespace

std::vector<std::string> missing_bootstrap_files(const Config& cfg,
                                                 const char* argv0,
                                                 bool require_completions) {
    std::vector<std::string> out;
    if (cfg.system_prompt_path.empty() ||
        resolve_data_path(cfg.system_prompt_path, argv0).empty())
        out.push_back(describe_missing("system prompt", "prompts/system.md",
                                       argv0));
    // An explicitly cleared tools prompt means the auto-rendered reference is
    // intended; only a configured-but-unfindable file is fatal.
    if (!cfg.tools_prompt_path.empty() &&
        resolve_data_path(cfg.tools_prompt_path, argv0).empty())
        out.push_back(
            describe_missing("tools prompt", "prompts/tools.md", argv0));
    if (require_completions &&
        resolve_data_path("completions.json", argv0).empty())
        out.push_back(describe_missing("command tree", "completions.json",
                                       argv0));
    return out;
}

} // namespace agent
