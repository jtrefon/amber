#ifndef AGENT_DATA_PATH_H
#define AGENT_DATA_PATH_H

#include <string>
#include <sys/stat.h>
#include <libgen.h>

namespace agent {

// Resolve a path that may be relative to the binary's directory. If the path
// exists as-is it is returned unchanged. Otherwise, if it is a relative path
// that does not exist in CWD but does exist relative to argv[0]'s directory,
// the binary-relative path is returned. This lets users run the binary from
// any working directory and still find data files bundled next to the binary.
inline std::string resolve_data_path(const std::string& path, const char* argv0) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return path;
    if (path.empty() || path[0] == '/' || !argv0) return path;
    std::string argv0_copy(argv0);
    char* dir = ::dirname(argv0_copy.data());
    std::string candidate = std::string(dir) + "/" + path;
    if (stat(candidate.c_str(), &st) == 0) return candidate;
    return path;
}

} // namespace agent

#endif // AGENT_DATA_PATH_H
