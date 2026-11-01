#include "agent/exit_ready.h"

#include <cstdlib>
#include <fstream>

namespace agent {

bool announce_exit_ready() {
    const char* path = std::getenv("AMBER_EXIT_READY_FILE");
    if (!path || !*path)
        return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "exit-ready\n";
    return static_cast<bool>(out);
}

} // namespace agent
