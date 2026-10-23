#ifndef AGENT_ARTIFACTS_H
#define AGENT_ARTIFACTS_H

#include <cstddef>
#include <optional>
#include <string>

namespace agent {

// A tool result too large for the context budget is written here and replaced
// by an opaque handle; the read_result tool pages and searches it. Artifacts
// are project-scoped state (under the XDG state dir), private to the user, and
// bounded by a retention policy that evicts the oldest first. Ids are
// validated, so no caller-supplied path ever reaches the filesystem.

inline constexpr std::size_t kArtifactMinBytes = std::size_t{4} * 1024;
inline constexpr std::size_t kArtifactMaxFiles = 64;
inline constexpr std::size_t kArtifactMaxBytes = std::size_t{32} * 1024 * 1024;
inline constexpr std::size_t kArtifactReadDefault = std::size_t{16} * 1024;
inline constexpr std::size_t kArtifactReadMax = std::size_t{64} * 1024;

// The project's artifact directory. Pure: just the path.
std::string artifact_dir();

bool valid_artifact_id(const std::string& id);

// Store a payload and return its handle; empty when it is too small, too big or
// the write failed (the envelope budget still protects the context).
std::string store_tool_artifact(const std::string& payload);

// A slice of a stored artifact. A non-empty `grep` returns matching lines
// prefixed with their line numbers instead of a byte range. Nullopt with a
// human-readable `error` when the id is unknown/expired or the read fails.
std::optional<std::string> read_tool_artifact(const std::string& id, std::size_t offset,
                                              std::size_t limit, const std::string& grep,
                                              std::string& error);

} // namespace agent

#endif // AGENT_ARTIFACTS_H
