#ifndef AGENT_DATA_PATH_H
#define AGENT_DATA_PATH_H

#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace agent {

// Ordered candidate locations for a data file, best first. The explicit path
// (CWD when relative), the binary's directory, the workspace root, user data
// dirs (XDG_DATA_HOME/amber, ~/.local/share/amber, legacy ~/.config/amber),
// and system data dirs (/usr/local/share/amber, /usr/share/amber). This keeps
// dev workflows (files next to the binary or in the project) working while
// letting packaged installs find system data from any working directory.
std::vector<std::string> data_file_candidates(const std::string& path,
                                              const char* argv0);

// First existing candidate, or "" when the file exists nowhere.
inline std::string resolve_data_path(const std::string& path,
                                     const char* argv0) {
    for (const auto& c : data_file_candidates(path, argv0)) {
        struct stat st;
        if (stat(c.c_str(), &st) == 0) return c;
    }
    return "";
}

inline bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Full path of the running binary (via /proc/self/exe), or "" if unknown.
std::string exe_dir();

} // namespace agent

#endif // AGENT_DATA_PATH_H
