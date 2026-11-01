#ifndef AGENT_LOG_RETENTION_H
#define AGENT_LOG_RETENTION_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace agent {

// One candidate for pruning, as the scan saw it. The planner never touches the
// filesystem, so the whole decision is testable with a fake clock.
struct LogEntry {
    std::string path;
    std::uint64_t size = 0;
    std::time_t mtime = 0;
};

// The bounds are "whichever is tighter": a file older than `days` goes, and the
// newest files are kept until the directory fits in `max_bytes`. The defaults
// are deliberately conservative -- an old transcript is regenerable from the
// session file, but losing a week of them is not something a user should
// discover by accident.
struct RetentionPolicy {
    int days = 14;
    std::uint64_t max_bytes = 256ull * 1024 * 1024;
};

struct LogStats {
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::time_t oldest = 0;
    std::time_t newest = 0;
};

struct RetentionReport {
    LogStats before;
    LogStats after;
    std::size_t removed = 0;
    std::vector<std::string> errors;
    // Set when the open transcript alone exceeds `max_bytes`: it is never
    // deleted, and the readout has to say so rather than claim the cap holds.
    bool active_over_cap = false;
};

// True for the files the policy manages: session transcripts (*.jsonl) and
// debug dumps (debug-*). The error journal is deliberately not one of them: it
// is small, the user turns it on on purpose, and it is the record of what went
// wrong. Sessions are user data and live outside the log directory.
bool is_prunable_log(const std::string& path);

// lstat-based scan of `dir`: regular files only, symlinks skipped (never
// followed) and subdirectories not descended. Sorted by path so a caller sees a
// stable order whatever the filesystem returns.
std::vector<LogEntry> scan_log_entries(const std::string& dir);

// Counts and bounds over a set of entries. Pure.
LogStats summarize_logs(const std::vector<LogEntry>& entries);

// The exact delete list, oldest first (ties broken by path so a run is
// reproducible). `active` names the transcripts this process has open; no entry
// in it is ever planned, whatever the bounds say.
std::vector<std::string> plan_log_prune(const std::vector<LogEntry>& entries,
                                        const RetentionPolicy& policy, std::time_t now,
                                        const std::vector<std::string>& active);

// Scans, plans, unlinks. Counts what it did and reports per-file failures
// instead of throwing: this runs on a worker at startup, where an exception has
// nowhere to go and a full disk is not a reason to refuse to start.
RetentionReport prune_logs(const std::string& dir, const RetentionPolicy& policy, std::time_t now,
                           const std::vector<std::string>& active);

} // namespace agent

#endif // AGENT_LOG_RETENTION_H
