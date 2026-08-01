
#ifndef AGENT_ARCHIVE_UTIL_H
#define AGENT_ARCHIVE_UTIL_H

#include <string>

namespace agent {

// Fetch bytes from an http(s) URL (via libcurl) or read a local file.
// Returns "" with `err` set on failure.
std::string fetch_bytes(const std::string& source, std::string& err);

// List the entries of a tar.gz archive (one per line), "" on failure.
// Used to validate archive contents before extraction.
std::string list_tar_gz(const std::string& archive_path);

// Extract a tar.gz archive into `dest`. Returns "" on success or a
// human-readable error.
std::string unpack_tar_gz(const std::string& archive_path,
                          const std::string& dest);

} // namespace agent

#endif // AGENT_ARCHIVE_UTIL_H
