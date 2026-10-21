#ifndef BENCH_STATE_REAPER_H
#define BENCH_STATE_REAPER_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bench {

// The project-state slug prefix a scenario workspace leaves behind:
// /tmp/amber_bench_ws_<name>_<pid> -> workspace_state_slug -> this prefix.
// Kept in one place so the reaper and the runner cannot drift apart.
inline constexpr const char* kScenarioStatePrefix = "tmp-amber-bench-ws-";

struct ReapOptions {
    int older_than_days = 7; // minimum 1; the CLI validates too
    bool apply = false;      // false = dry run
};

struct ReapEntry {
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
};

struct ReapReport {
    std::vector<ReapEntry> entries;
    std::uintmax_t bytes = 0;
    std::size_t skipped_young = 0;
    std::vector<std::string> errors;
};

// Select (and with options.apply, remove) direct children of projects_root
// whose names carry kScenarioStatePrefix and whose mtime is older than the
// age floor. Everything else is left alone. A missing root is empty, not an
// error; filesystem failures are collected in errors.
ReapReport reap_scenario_state(const std::filesystem::path& projects_root,
                               const ReapOptions& options);

} // namespace bench

#endif // BENCH_STATE_REAPER_H
