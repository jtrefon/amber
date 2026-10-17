
#ifndef AGENT_CONFIG_PATHS_H
#define AGENT_CONFIG_PATHS_H

#include <string>
#include <vector>

namespace agent {

// One row of amber's own file layout.
struct ConfigPath {
    std::string what; // short name: "providers", "sessions", ...
    std::string path; // resolved location
    std::string note; // what lives there, and what writes it
};

// Amber's own layout, resolved from the environment (XDG overrides included).
//
// One source of truth for two readers that must never disagree: the
// `/get config paths` readout a user (or the agent, through bash) reads, and
// the bundled self-knowledge skill, whose body is rendered from this table.
// A path restated in prose would drift the first time the layout moves; a path
// rendered from here cannot.
//
// Nothing is created: this is what amber WOULD use, not what exists yet.
std::vector<ConfigPath> config_paths();

// The error journal's path, beside the per-session transcripts. Resolved here
// so the readout row and the writer cannot disagree about where it lives; the
// file itself appears only when diagnostics are on and something went wrong.
std::string error_log_path();

} // namespace agent

#endif // AGENT_CONFIG_PATHS_H
