#include "agent/log_retention.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace agent {

namespace {

namespace fs = std::filesystem;

constexpr std::time_t kSecondsPerDay = 24LL * 60 * 60;

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_active(const std::string& path, const std::vector<std::string>& active) {
    return std::find(active.begin(), active.end(), path) != active.end();
}

std::uint64_t total_bytes(const std::vector<FileEntry>& entries) {
    std::uint64_t total = 0;
    for (const auto& entry : entries)
        total += entry.size;
    return total;
}

// The managed entries in deletion order: oldest first, ties broken by path so
// two runs over the same directory cannot disagree.
std::vector<FileEntry> managed_oldest_first(const std::vector<FileEntry>& entries,
                                            const std::vector<std::string>& active) {
    std::vector<FileEntry> managed;
    for (const auto& entry : entries) {
        if (is_prunable_log(entry.path) && !is_active(entry.path, active))
            managed.push_back(entry);
    }
    std::sort(managed.begin(), managed.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.mtime != b.mtime)
            return a.mtime < b.mtime;
        return a.path < b.path;
    });
    return managed;
}

} // namespace

bool is_prunable_log(const std::string& path) {
    const std::string name = fs::path(path).filename().string();
    // The journal is a log too, but a deliberate one: it is small, it is on
    // because the user asked for it, and it is the record of what went wrong.
    if (name == "errors.jsonl")
        return false;
    return ends_with(name, ".jsonl") || name.rfind("debug-", 0) == 0;
}

std::vector<FileEntry> scan_regular_files(const std::string& dir) {
    std::vector<FileEntry> entries;
    std::error_code ec;
    const fs::directory_options options = fs::directory_options::skip_permission_denied;
    fs::directory_iterator it(dir, options, ec);
    if (ec)
        return entries;
    for (const auto& item : it) {
        // lstat, never stat: a symlink is skipped rather than followed, so a
        // link planted in the log directory cannot reach a file outside it.
        struct stat info {};
        if (::lstat(item.path().c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        FileEntry entry;
        entry.path = item.path().string();
        entry.size = static_cast<std::uint64_t>(info.st_size);
        entry.mtime = info.st_mtime;
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    return entries;
}

FileStats summarize_files(const std::vector<FileEntry>& entries) {
    FileStats stats;
    stats.files = entries.size();
    for (const auto& entry : entries) {
        stats.bytes += entry.size;
        if (stats.oldest == 0 || entry.mtime < stats.oldest)
            stats.oldest = entry.mtime;
        if (entry.mtime > stats.newest)
            stats.newest = entry.mtime;
    }
    return stats;
}

std::vector<std::string> plan_log_prune(const std::vector<FileEntry>& entries,
                                        const RetentionPolicy& policy, std::time_t now,
                                        const std::vector<std::string>& active) {
    std::vector<FileEntry> managed = managed_oldest_first(entries, active);
    std::vector<std::string> plan;

    // The age bound first: everything older than it goes, and the managed set is
    // already in oldest-first order, so the plan stays ordered.
    if (policy.days > 0) {
        const std::time_t cutoff = now - (static_cast<std::time_t>(policy.days) * kSecondsPerDay);
        auto it = managed.begin();
        while (it != managed.end() && it->mtime < cutoff) {
            plan.push_back(it->path);
            it = managed.erase(it);
        }
    }

    // Then the size bound, still oldest first. Active transcripts count toward
    // the total -- they occupy the directory -- but are not in `managed`, so the
    // loop stops when only they are left.
    std::uint64_t total = total_bytes(managed);
    for (const auto& entry : entries) {
        if (!is_prunable_log(entry.path))
            continue;
        if (is_active(entry.path, active))
            total += entry.size;
    }
    for (const auto& entry : managed) {
        if (total <= policy.max_bytes)
            break;
        plan.push_back(entry.path);
        total -= entry.size;
    }
    return plan;
}

RetentionReport prune_logs(const std::string& dir, const RetentionPolicy& policy, std::time_t now,
                           const std::vector<std::string>& active) {
    RetentionReport report;
    const std::vector<FileEntry> before = scan_regular_files(dir);
    report.before = summarize_files(before);
    for (const auto& entry : before) {
        if (is_active(entry.path, active) && entry.size > policy.max_bytes)
            report.active_over_cap = true;
    }

    for (const std::string& path : plan_log_prune(before, policy, now, active)) {
        // A racing process may have removed it already; that is the outcome this
        // run wanted, not a failure.
        if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
            ++report.removed;
            continue;
        }
        report.errors.push_back(path + ": " + std::strerror(errno));
    }
    report.after = summarize_files(scan_regular_files(dir));
    return report;
}

} // namespace agent
