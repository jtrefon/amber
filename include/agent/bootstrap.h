#ifndef AGENT_BOOTSTRAP_H
#define AGENT_BOOTSTRAP_H

#include <string>
#include <vector>

namespace agent {

struct Config;

// Pre-flight check run by the hosts (CLI/TUI) before the UI starts. Returns
// human-readable descriptions of every critical data file that cannot be
// found ("system prompt: prompts/system.md; searched: …"). Empty result means
// bootstrap is safe. `require_completions` is set by the TUI, which needs the
// command tree; the headless CLI does not.
std::vector<std::string> missing_bootstrap_files(const Config& cfg,
                                                 const char* argv0,
                                                 bool require_completions);

} // namespace agent

#endif // AGENT_BOOTSTRAP_H
