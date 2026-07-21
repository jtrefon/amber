// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/debug_log.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <set>

namespace agent {

namespace {

// Paths already truncated at process start. The debug log lives at a single
// fixed path and is truncated (not appended) on the first write of the
// process, so each launch auto-clears the previous run's output and the file
// can never grow unbounded across restarts.
std::mutex g_truncated_mu;
std::set<std::string> g_truncated;

} // namespace

void debug_log(const std::string& path, const std::string& tag,
               const std::string& payload) {
    if (path.empty()) return;
    std::ios::openmode mode = std::ios::app | std::ios::binary;
    {
        std::scoped_lock lk(g_truncated_mu);
        if (g_truncated.find(path) == g_truncated.end()) {
            mode = std::ios::trunc | std::ios::binary;
            g_truncated.insert(path);
        }
    }
    std::ofstream f(path, mode);
    if (!f) return;
    using namespace std::chrono;
    long long ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count();
    f << "==== " << ms << ' ' << tag << " (" << payload.size() << "B) ====\n"
      << payload << "\n";
}

} // namespace agent
