#include "agent/exit_ready.h"

#include "agent/config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

namespace agent {

bool announce_exit_ready() {
    // Presence is the signal; the path is the process's own. Nothing here is
    // taken from the environment, so a manipulated value cannot redirect the
    // write the exit path performs.
    if (!std::getenv("AMBER_EXIT_READY"))
        return false;
    // The cache root, not the project state dir: a session can switch project
    // (a worktree, a project window), and the announcement is about this
    // process, not about whichever project it happened to finish in.
    const std::string dir = cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;
    std::ofstream out(dir + "/exit-ready-" + std::to_string(::getpid()), std::ios::trunc);
    if (!out)
        return false;
    out << "exit-ready\n";
    return static_cast<bool>(out);
}

} // namespace agent
