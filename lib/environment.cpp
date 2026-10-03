
#include "agent/environment.h"
#include "agent/workspace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

namespace agent {

namespace {

std::string shell_out(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p)
        return "";
    std::array<char, 512> buf{};
    std::string out;
    while (fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// True when `cmd` exits 0; false on spawn failure or non-zero status.
bool shell_ok(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p)
        return false;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), p)) {
    }
    int rc = pclose(p);
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

std::string distro_name() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0)
            continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

std::string os_string() {
    struct utsname u {};
    if (uname(&u) != 0)
        return "";
    std::string out = distro_name();
    if (!out.empty())
        out += " (";
    out += u.sysname;
    out += " ";
    out += u.release;
    out += " ";
    out += u.machine;
    if (!distro_name().empty())
        out += ")";
    return out;
}

std::string user_string() {
    if (const char* user = std::getenv("USER")) {
        if (*user)
            return user;
    }
    if (struct passwd* pw = getpwuid(getuid()))
        return pw->pw_name;
    return "";
}

std::string resources_string() {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1)
        nproc = 1;
    double gb = 0.0;
#ifdef __APPLE__
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0)
        gb = static_cast<double>(memsize) / (1024.0 * 1024.0 * 1024.0);
#else
    struct sysinfo si {};
    if (sysinfo(&si) == 0)
        gb = static_cast<double>(si.totalram) * static_cast<double>(si.mem_unit) /
             (1024.0 * 1024.0 * 1024.0);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%ld cores \u00b7 %.0f GB RAM", nproc, gb);
    return buf;
}

std::string date_string() {
    std::time_t t = std::time(nullptr);
    struct tm tm {};
    if (!localtime_r(&t, &tm))
        return "";
    char buf[32];
    if (!std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm))
        return "";
    return buf;
}

std::string timezone_string() {
    std::time_t t = std::time(nullptr);
    struct tm tm {};
    if (!localtime_r(&t, &tm))
        return "";
    long off = tm.tm_gmtoff;
    int hours = static_cast<int>(off / 3600);
    std::string s = "UTC";
    if (hours >= 0)
        s += "+";
    s += std::to_string(hours);
    return s;
}

std::vector<std::string> present_tools() {
    static const char* kTools[] = {"git",    "python3", "node",    "npm",  "make", "cmake",
                                   "gcc",    "g++",     "clang++", "curl", "jq",   "tree",
                                   "docker", "tmux",    "rg",      "fd"};
    // One `command -v` per tool: dash only prints the first match when given
    // several arguments at once.
    std::string cmd = "for t in";
    for (const char* t : kTools) {
        cmd += " ";
        cmd += t;
    }
    cmd += "; do command -v \"$t\" 2>/dev/null; done";
    std::string out = shell_out(cmd);
    std::vector<std::string> names;
    std::istringstream ss(out);
    std::string path;
    while (ss >> path) {
        std::string base = path.substr(path.find_last_of('/') + 1);
        if (!base.empty())
            names.push_back(base);
    }
    return names;
}

} // namespace

EnvironmentInfo probe_environment() {
    EnvironmentInfo info;
    info.os = os_string();
    info.user = user_string();
    info.root = (geteuid() == 0);
    if (!info.root)
        info.sudo_passwordless = shell_ok("sudo -n true 2>/dev/null");
    info.workspace = Workspace::root();
    info.date = date_string();
    info.timezone = timezone_string();
    info.resources = resources_string();
    info.tools = present_tools();
    return info;
}

namespace {

// The user line, which has three forms: root, non-root with passwordless sudo,
// and plain non-root.
std::string environment_user_line(const EnvironmentInfo& info) {
    if (info.root)
        return "User: root";
    const std::string role = info.sudo_passwordless ? "non-root, passwordless sudo" : "non-root";
    return "User: " + info.user + " (" + role + ")";
}

std::string environment_date_line(const EnvironmentInfo& info) {
    std::string line = "Date: " + info.date;
    if (!info.timezone.empty())
        line += " (" + info.timezone + ")";
    return line;
}

// The tool list, sorted and comma-joined.
std::string join_sorted(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i)
            out += ", ";
        out += items[i];
    }
    return out;
}

} // namespace

std::string render_environment_card(const EnvironmentInfo& info) {
    std::string card = "## Environment";
    bool any = false;
    auto add = [&](const std::string& line) {
        card += "\n- ";
        card += line;
        any = true;
    };
    if (!info.os.empty())
        add("OS: " + info.os);
    if (!info.workspace.empty())
        add("Workspace: " + info.workspace + " (bash commands run here)");
    if (!info.user.empty())
        add(environment_user_line(info));
    if (!info.date.empty())
        add(environment_date_line(info));
    if (!info.resources.empty())
        add("Resources: " + info.resources);
    if (!info.tools.empty())
        add("Tools available: " + join_sorted(info.tools));
    return any ? card : "";
}

} // namespace agent
