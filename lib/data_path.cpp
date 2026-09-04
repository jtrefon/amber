
#include "agent/data_path.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace agent {

namespace {

std::string dirname_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME")) return h;
    return "";
}

void add_if_unique(std::vector<std::string>& out, std::string dir,
                   const std::string& path) {
    if (dir.empty() || dir == ".") return;
    if (dir.back() != '/') dir += "/";
    std::string candidate = dir + path;
    if (std::find(out.begin(), out.end(), candidate) == out.end())
        out.push_back(std::move(candidate));
}

} // namespace

std::string exe_dir() {
    std::array<char, 4096> buf{};
#ifdef __APPLE__
    uint32_t size = buf.size() - 1;
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return "";
    std::array<char, 4096> resolved{};
    if (realpath(buf.data(), resolved.data()) == nullptr) return "";
    return dirname_of(resolved.data());
#else
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    return dirname_of(buf.data());
#endif
}

std::vector<std::string> data_file_candidates(const std::string& path,
                                              const char* argv0) {
    std::vector<std::string> out;
    if (path.empty()) return out;
    // 1. As given — CWD when relative.
    if (path[0] == '/' || file_exists(path))
        out.push_back(path);
    // 2. Next to the binary, and its sibling FHS share dir
    //    (<prefix>/bin/amber + <prefix>/share/amber). This keeps dev
    //    workflows working and makes packaged installs relocatable to any
    //    prefix — Homebrew on Intel (/usr/local) and Apple Silicon
    //    (/opt/homebrew), or a custom --prefix.
    if (argv0) {
        std::string bindir = dirname_of(argv0);
        // A bare argv0 (no slash) means the binary was launched via PATH;
        // resolve the real executable path so a relocatable install prefix
        // (Homebrew, /usr/local, custom) can be found next to it.
        if (std::string(argv0).find('/') == std::string::npos) {
            std::string real = exe_dir();
            if (!real.empty()) bindir = real;
        }
        add_if_unique(out, bindir, path);
        if (bindir != ".") add_if_unique(out, bindir + "/../share/amber", path);
    }
    // 3. Workspace root.
    if (const char* ws = std::getenv("AMBER_WORKSPACE"))
        add_if_unique(out, ws, path);
    // 4. User data dirs.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        add_if_unique(out, std::string(xdg) + "/amber", path);
    std::string home = home_dir();
    if (!home.empty()) {
        add_if_unique(out, home + "/.local/share/amber", path);
        add_if_unique(out, home + "/.config/amber", path);
    }
    // 5. System data dirs.
    add_if_unique(out, "/usr/local/share/amber", path);
    add_if_unique(out, "/usr/share/amber", path);
    return out;
}

} // namespace agent
