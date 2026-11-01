#include "agent/exit_ready.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace agent {

namespace {

bool write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

} // namespace

bool announce_exit_ready(int fd) {
    // Presence is the signal. Nothing here is taken from the environment, so no
    // value can steer where this writes.
    if (!std::getenv("AMBER_EXIT_READY"))
        return false;
    // Leading newline: the marker starts on its own line whatever the last
    // painted row left behind, and the harness matches the text, not the cursor.
    const std::string line = std::string("\n") + kExitReadyMarker + "\n";
    return write_all(fd, line.data(), line.size());
}

} // namespace agent
