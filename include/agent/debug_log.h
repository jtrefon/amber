
#ifndef AGENT_DEBUG_LOG_H
#define AGENT_DEBUG_LOG_H

#include <string>

namespace agent {

// Append a single labelled record to the debug log at `path`. No-op when the
// path is empty. Binary-safe: writes raw bytes verbatim (useful for raw SSE
// dumps). The "{ts}" placeholder in the path is resolved to a process-start
// stamp so concurrent streams land in distinct files.
void debug_log(const std::string& path, const std::string& tag,
               const std::string& payload);

} // namespace agent

#endif // AGENT_DEBUG_LOG_H
