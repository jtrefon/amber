// sysinfo plugin — host facts for the agent (memory, CPU, partitions, network).
// Standalone executable speaking the amber plugin protocol (JSON-RPC 2.0,
// newline-delimited over stdio). Intentionally dependency-free: reads /proc on
// Linux, sysctl/getfsstat/getifaddrs on macOS/BSD.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#ifdef __APPLE__
#include <net/if_dl.h>  // AF_LINK (link-layer address family)
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>  // struct loadavg is declared here on Darwin
#include <sys/ucred.h>
#endif

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

json result(bool ok, const std::string& output, const json& meta = json::object()) {
    json r = {{"ok", ok}, {"output", output}, {"meta", meta}};
    return r;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

#ifndef __APPLE__
// Reads a /proc file fully.
std::string read_proc(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
#endif

std::string tool_mem() {
#ifdef __APPLE__
    uint64_t total = 0;
    size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) != 0)
        return "ERROR: cannot query hw.memsize";
    uint64_t page_size = 0, free_pages = 0, inactive_pages = 0;
    len = sizeof page_size;
    sysctlbyname("vm.pagesize", &page_size, &len, nullptr, 0);
    len = sizeof free_pages;
    sysctlbyname("vm.page_free_count", &free_pages, &len, nullptr, 0);
    len = sizeof inactive_pages;
    sysctlbyname("vm.page_inactive_count", &inactive_pages, &len, nullptr, 0);
    const double total_mb = static_cast<double>(total) / 1048576.0;
    const double avail_mb =
        (static_cast<double>(free_pages + inactive_pages) *
         static_cast<double>(page_size)) /
        1048576.0;
    struct xsw_usage swap{};
    len = sizeof swap;
    const bool have_swap =
        sysctlbyname("vm.swapusage", &swap, &len, nullptr, 0) == 0;
    std::ostringstream out;
    out << "memory total " << total_mb << " MB, used " << (total_mb - avail_mb)
        << " MB, free " << avail_mb << " MB, available " << avail_mb << " MB\n";
    if (have_swap)
        out << "swap total " << swap.xsu_total / 1048576.0 << " MB, used "
            << swap.xsu_used / 1048576.0 << " MB";
    return out.str();
#else
    std::string raw = read_proc("/proc/meminfo");
    if (raw.empty()) return "ERROR: cannot read /proc/meminfo";
    long total_kb = 0, free_kb = 0, avail_kb = 0, swap_total = 0, swap_free = 0;
    std::istringstream ss(raw);
    std::string key, unit;
    long value;
    while (ss >> key >> value >> unit) {
        key.pop_back();  // trailing ':'
        if (key == "MemTotal") total_kb = value;
        else if (key == "MemFree") free_kb = value;
        else if (key == "MemAvailable") avail_kb = value;
        else if (key == "SwapTotal") swap_total = value;
        else if (key == "SwapFree") swap_free = value;
    }
    auto mb = [](long kb) { return kb / 1024.0; };
    std::ostringstream out;
    out << "memory total " << mb(total_kb) << " MB, used " << mb(total_kb - avail_kb)
        << " MB, free " << mb(free_kb) << " MB, available " << mb(avail_kb) << " MB\n"
        << "swap total " << mb(swap_total) << " MB, used "
        << mb(swap_total - swap_free) << " MB";
    return out.str();
#endif
}

std::string tool_cpu() {
#ifdef __APPLE__
    struct loadavg la{};
    size_t len = sizeof la;
    long cores = 0;
    size_t clen = sizeof cores;
    if (sysctlbyname("vm.loadavg", &la, &len, nullptr, 0) != 0)
        return "ERROR: cannot query vm.loadavg";
    if (sysctlbyname("hw.activecpu", &cores, &clen, nullptr, 0) != 0) cores = 0;
    std::ostringstream out;
    out << "load average 1/5/15 min: "
        << static_cast<double>(la.ldavg[0]) / la.fscale << " / "
        << static_cast<double>(la.ldavg[1]) / la.fscale << " / "
        << static_cast<double>(la.ldavg[2]) / la.fscale << " on " << cores
        << " cores";
    return out.str();
#else
    std::string raw = read_proc("/proc/loadavg");
    if (raw.empty()) return "ERROR: cannot read /proc/loadavg";
    std::istringstream ss(raw);
    double l1, l5, l15;
    ss >> l1 >> l5 >> l15;
    long cores = 0;
    std::istringstream ss2(read_proc("/proc/stat"));
    std::string l;
    while (std::getline(ss2, l))
        if (l.rfind("cpu", 0) == 0) ++cores;
    std::ostringstream out;
    out << "load average 1/5/15 min: " << l1 << " / " << l5 << " / " << l15
        << " on " << (cores > 0 ? cores - 1 : 0) << " cores";
    return out.str();
#endif
}

std::string tool_partitions() {
#ifdef __APPLE__
    int count = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0) return "ERROR: cannot enumerate mounted filesystems";
    std::vector<struct statfs> fs(static_cast<size_t>(count));
    count = getfsstat(fs.data(),
                      static_cast<int>(fs.size() * sizeof(struct statfs)),
                      MNT_NOWAIT);
    if (count <= 0) return "ERROR: cannot enumerate mounted filesystems";
    std::ostringstream out;
    for (int i = 0; i < count; ++i) {
        const std::string mnt = fs[i].f_mntonname;
        // macOS read-only firmware/preboot/VM mounts duplicate the Data
        // volume; report the real filesystems only.
        if (mnt.rfind("/System/Volumes/", 0) == 0 && mnt != "/System/Volumes/Data")
            continue;
        out << fs[i].f_mntfromname << " mounted on " << mnt << " "
            << static_cast<double>(static_cast<uint64_t>(fs[i].f_bsize) *
                                   fs[i].f_blocks) /
                   1073741824.0
            << " GB\n";
    }
    return trim(out.str());
#else
    std::string raw = read_proc("/proc/partitions");
    if (raw.empty()) return "ERROR: cannot read /proc/partitions";
    std::istringstream ss(raw);
    std::string line;
    std::ostringstream out;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        long major = 0, minor = 0, blocks = 0;
        std::string name;
        if (ls >> major >> minor >> blocks >> name) {
            if (name.empty() || name[0] == '.') continue;
            out << name << " " << blocks / 1024.0 / 1024.0 << " GB\n";
        }
    }
    return trim(out.str());
#endif
}

std::string tool_net() {
#ifdef __APPLE__
    std::ostringstream out;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return "ERROR: getifaddrs failed";
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        // Skip the link-layer address: AF_PACKET on Linux, AF_LINK on
        // macOS/BSD.
        if (!p->ifa_addr || p->ifa_addr->sa_family == AF_LINK) continue;
        char buf[INET6_ADDRSTRLEN] = {};
        void* src = nullptr;
        if (p->ifa_addr->sa_family == AF_INET)
            src = &reinterpret_cast<struct sockaddr_in*>(p->ifa_addr)->sin_addr;
        else if (p->ifa_addr->sa_family == AF_INET6)
            src = &reinterpret_cast<struct sockaddr_in6*>(p->ifa_addr)->sin6_addr;
        if (src && inet_ntop(p->ifa_addr->sa_family, src, buf, sizeof buf))
            out << p->ifa_name << " " << buf << "\n";
    }
    freeifaddrs(ifa);
    return trim(out.str());
#else
    std::ifstream dev("/proc/net/dev");
    if (!dev) return "ERROR: cannot read /proc/net/dev";
    std::string line;
    std::getline(dev, line);
    std::getline(dev, line);
    std::ostringstream out;
    while (std::getline(dev, line)) {
        std::istringstream ls(line);
        std::string name;
        ls >> name;
        name.pop_back();  // trailing ':'
        if (name == "lo") continue;
        long rx = 0, tx = 0;
        ls >> rx;
        for (int i = 1; i < 8; ++i) { long x; ls >> x; }
        ls >> tx;
        out << name << ": rx " << rx << " bytes, tx " << tx << " bytes\n";
    }

    // Addresses via getifaddrs.
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
            // Skip the link-layer address: AF_PACKET on Linux, AF_LINK on
            // macOS/BSD.
            if (!p->ifa_addr || p->ifa_addr->sa_family == AF_PACKET) continue;
            char buf[INET6_ADDRSTRLEN] = {};
            void* src = nullptr;
            if (p->ifa_addr->sa_family == AF_INET)
                src = &reinterpret_cast<struct sockaddr_in*>(p->ifa_addr)->sin_addr;
            else if (p->ifa_addr->sa_family == AF_INET6)
                src = &reinterpret_cast<struct sockaddr_in6*>(p->ifa_addr)->sin6_addr;
            if (src && inet_ntop(p->ifa_addr->sa_family, src, buf, sizeof buf))
                out << p->ifa_name << " " << buf << "\n";
        }
        freeifaddrs(ifa);
    }
    return trim(out.str());
#endif
}

json dispatch(const std::string& name, const json& args) {
    (void)args;
    if (name == "mem") return result(true, tool_mem());
    if (name == "cpu") return result(true, tool_cpu());
    if (name == "partitions") return result(true, tool_partitions());
    if (name == "net") return result(true, tool_net());
    return result(false, "ERROR: unknown tool " + name);
}

} // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded()) continue;
            json resp = json::object();
            std::string method = msg.value("method", std::string());
            resp["id"] = msg.value("id", json());
            if (method == "initialize") {
                resp["result"] = {{"protocol_version", 1}, {"ok", true}};
            } else if (method == "tool.call") {
                resp["result"] = dispatch(
                    msg["params"].value("name", std::string()),
                    msg["params"].contains("args") ? msg["params"]["args"] : json::object());
            } else if (method == "shutdown") {
                break;
            } else {
                std::string err = "ERROR: unknown method ";
                err += method;
                resp["result"] = result(false, err);
            }
            std::cout << resp.dump() << "\n";
            std::cout.flush();
        } catch (const std::exception& e) {
            std::string err = "ERROR: ";
            err += e.what();
            std::cout << json{{"id", json()}, {"result", result(false, err)}}.dump()
                      << "\n";
            std::cout.flush();
        }
    }
    return 0;
}