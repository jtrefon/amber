
#include "agent/environment.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace agent {

namespace {

std::string shell_out(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::array<char, 512> buf{};
    std::string out;
    while (fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    pclose(p);
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string distro_name() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

std::string os_string() {
    struct utsname u {};
    if (uname(&u) != 0) return "Linux";
    std::string out = distro_name();
    if (!out.empty()) out += " (";
    out += "Linux ";
    out += u.release;
    out += " ";
    out += u.machine;
    if (!distro_name().empty()) out += ")";
    return out;
}

std::string user_host_string() {
    std::string out;
    if (const char* user = std::getenv("USER")) out = user;
    if (out.empty()) {
        if (struct passwd* pw = getpwuid(getuid())) out = pw->pw_name;
    }
    char host[256] = {};
    if (gethostname(host, sizeof host) == 0) {
        if (!out.empty()) out += "@";
        out += host;
    }
    return out;
}

std::string resources_string() {
    struct sysinfo si {};
    if (sysinfo(&si) != 0) return "";
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
    double gb = static_cast<double>(si.totalram) *
                static_cast<double>(si.mem_unit) / (1024.0 * 1024.0 * 1024.0);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%ld cores \u00b7 %.0f GB RAM", nproc, gb);
    return buf;
}

std::vector<std::string> present_tools() {
    static const char* kTools[] = {"git", "python3", "node",   "npm",
                                   "make", "cmake",   "gcc",    "g++",
                                   "clang++", "curl", "jq",     "tree",
                                   "docker", "tmux"};
    std::string cmd = "command -v";
    for (const char* t : kTools) {
        cmd += " ";
        cmd += t;
    }
    std::string out = shell_out(cmd);
    std::vector<std::string> names;
    std::istringstream ss(out);
    std::string path;
    while (ss >> path) {
        std::string base = path.substr(path.find_last_of('/') + 1);
        if (!base.empty()) names.push_back(base);
    }
    return names;
}

} // namespace

EnvironmentInfo probe_environment() {
    EnvironmentInfo info;
    info.os = os_string();
    info.user_host = user_host_string();
    char buf[4096];
    if (getcwd(buf, sizeof buf)) info.cwd = buf;
    info.resources = resources_string();
    info.tools = present_tools();
    return info;
}

std::string render_environment_card(const EnvironmentInfo& info) {
    std::string card = "## Environment";
    bool any = false;
    auto add = [&](const std::string& line) {
        card += "\n- ";
        card += line;
        any = true;
    };
    if (!info.os.empty()) add("OS: " + info.os);
    if (!info.user_host.empty()) add("User: " + info.user_host);
    if (!info.cwd.empty()) add("Working directory: " + info.cwd);
    if (!info.resources.empty()) add("Resources: " + info.resources);
    if (!info.tools.empty()) {
        std::string t;
        for (size_t i = 0; i < info.tools.size(); ++i) {
            if (i) t += ", ";
            t += info.tools[i];
        }
        add("Tools available: " + t);
    }
    return any ? card : "";
}

} // namespace agent
