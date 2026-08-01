
#ifndef AGENT_ENVIRONMENT_H
#define AGENT_ENVIRONMENT_H

#include <string>
#include <vector>

namespace agent {

// Facts about the machine the agent runs on, collected once at session start
// and rendered into the system prompt (session-fixed, stable KV prefix).
// Kept compact: a handful of lines, no probe failure is fatal — unknown
// fields are simply omitted.
struct EnvironmentInfo {
    std::string os;             // e.g. "Ubuntu 24.04 (Linux 6.8.0-45 x86_64)"
    std::string user_host;      // "jack@box"
    std::string cwd;            // working directory
    std::string resources;      // "8 cores \u00b7 16 GB RAM"
    std::vector<std::string> tools;  // present tools, e.g. {"git","python3"}
};

// Collect environment facts. Never throws; missing probes leave fields empty.
EnvironmentInfo probe_environment();

// Render the card for the system prompt; empty fields are omitted. Returns ""
// when nothing was collected.
std::string render_environment_card(const EnvironmentInfo& info);

} // namespace agent

#endif // AGENT_ENVIRONMENT_H
