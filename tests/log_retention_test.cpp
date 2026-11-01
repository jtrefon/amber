// Log retention: the decision is pure and testable with a fake clock, the delete
// is lstat-based and never follows a symlink, and the transcript this process
// has open is never a candidate.

#include "agent/log_retention.h"

#include "test_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace agent;

namespace {

namespace fs = std::filesystem;

constexpr std::time_t kNow = 1800000000;
constexpr std::time_t kDay = 24 * 60 * 60;

// A private directory with helpers that pin size and mtime, so no assertion
// depends on the wall clock.
struct LogDir {
    fs::path dir;

    LogDir() {
        static int serial = 0;
        dir = fs::temp_directory_path() / ("amber-log-retention-" + std::to_string(::getpid()) +
                                           "-" + std::to_string(++serial));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~LogDir() {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
    }
    LogDir(const LogDir&) = delete;
    LogDir& operator=(const LogDir&) = delete;

    std::string add(const std::string& name, std::size_t size, std::time_t mtime) {
        const fs::path path = dir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << std::string(size, 'x');
        out.close();
        const timespec times[2] = {{mtime, 0}, {mtime, 0}};
        ::utimensat(AT_FDCWD, path.c_str(), times, 0);
        return path.string();
    }
};

std::vector<std::string> plan_for(const LogDir& logs, const RetentionPolicy& policy,
                                  const std::vector<std::string>& active = {}) {
    return plan_log_prune(scan_log_entries(logs.dir.string()), policy, kNow, active);
}

RetentionPolicy policy_of(int days, std::uint64_t max_bytes) {
    RetentionPolicy policy;
    policy.days = days;
    policy.max_bytes = max_bytes;
    return policy;
}

} // namespace

TEST(plan_prune_keeps_newest_by_age) {
    LogDir logs;
    const std::string oldest = logs.add("oldest.jsonl", 10, kNow - 20 * kDay);
    const std::string older = logs.add("older.jsonl", 10, kNow - 15 * kDay);
    logs.add("middle.jsonl", 10, kNow - 10 * kDay);
    logs.add("recent.jsonl", 10, kNow - 2 * kDay);
    logs.add("newest.jsonl", 10, kNow - kDay);

    const auto plan = plan_for(logs, RetentionPolicy{});
    ASSERT_TRUE(plan == std::vector<std::string>({oldest, older}));
}

TEST(plan_prune_enforces_size_bound_oldest_first) {
    LogDir logs;
    const std::string first = logs.add("a.jsonl", 100, kNow - 5 * kDay);
    const std::string second = logs.add("b.jsonl", 100, kNow - 4 * kDay);
    const std::string third = logs.add("c.jsonl", 100, kNow - 3 * kDay);
    logs.add("d.jsonl", 100, kNow - 2 * kDay);
    logs.add("e.jsonl", 100, kNow - kDay);

    // 500 bytes against a 250-byte cap: three go, two stay.
    const auto plan = plan_for(logs, policy_of(3650, 250));
    ASSERT_TRUE(plan == std::vector<std::string>({first, second, third}));
}

TEST(plan_prune_never_deletes_active_even_over_cap) {
    LogDir logs;
    const std::string other = logs.add("other.jsonl", 100, kNow - 2 * kDay);
    const std::string active = logs.add("active.jsonl", 600, kNow - kDay);

    const auto plan = plan_for(logs, policy_of(3650, 100), {active});
    // The active transcript is untouchable; the rest shrinks as far as it can.
    ASSERT_TRUE(plan == std::vector<std::string>({other}));

    const RetentionReport report =
        prune_logs(logs.dir.string(), policy_of(3650, 100), kNow, {active});
    ASSERT_TRUE(report.active_over_cap);
    ASSERT_TRUE(std::filesystem::exists(active));
    ASSERT_EQ(report.removed, 1u);
}

TEST(plan_prune_is_stable_on_equal_mtimes) {
    LogDir logs;
    const std::string bravo = logs.add("bravo.jsonl", 100, kNow - 20 * kDay);
    const std::string alpha = logs.add("alpha.jsonl", 100, kNow - 20 * kDay);

    // Same age: the path breaks the tie, so two runs cannot disagree.
    const auto plan = plan_for(logs, policy_of(1, 1));
    ASSERT_TRUE(plan == std::vector<std::string>({alpha, bravo}));
}

TEST(plan_prune_leaves_the_error_journal_alone) {
    LogDir logs;
    const std::string journal = logs.add("errors.jsonl", 100, kNow - 90 * kDay);
    const std::string transcript = logs.add("t.jsonl", 100, kNow - 30 * kDay);

    const auto plan = plan_for(logs, RetentionPolicy{});
    ASSERT_TRUE(plan == std::vector<std::string>({transcript}));
    ASSERT_TRUE(std::filesystem::exists(journal));
}

TEST(scan_logs_skips_symlinks_and_subdirectories) {
    LogDir logs;
    const std::string outside = logs.add("outside.jsonl", 40, kNow);
    const fs::path link = logs.dir / "link.jsonl";
    std::filesystem::create_symlink(outside, link);
    std::filesystem::create_directories(logs.dir / "nested");
    logs.add("real.jsonl", 60, kNow);

    const auto entries = scan_log_entries(logs.dir.string());
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(entries[0].path, (logs.dir / "real.jsonl").string());

    // A prune over the directory cannot reach through the link either.
    const RetentionReport report = prune_logs(logs.dir.string(), policy_of(1, 0), kNow, {});
    ASSERT_TRUE(std::filesystem::exists(outside));
    ASSERT_EQ(report.errors.size(), 0u);
}

TEST(prune_logs_reports_errors_without_throwing) {
    LogDir logs;
    logs.add("a.jsonl", 100, kNow - 30 * kDay);
    std::error_code ec;
    fs::permissions(logs.dir, fs::perms::owner_read | fs::perms::owner_exec, ec);
    ASSERT_FALSE(ec);

    // A read-only directory makes the unlink fail; the report says so and the
    // caller keeps running.
    const RetentionReport report = prune_logs(logs.dir.string(), RetentionPolicy{}, kNow, {});
    ASSERT_EQ(report.removed, 0u);
    ASSERT_EQ(report.errors.size(), 1u);
    fs::permissions(logs.dir, fs::perms::owner_all, ec);
}

TEST(scan_logs_reports_count_bytes_and_bounds) {
    LogDir logs;
    logs.add("a.jsonl", 10, kNow - 3 * kDay);
    logs.add("b.jsonl", 20, kNow - 2 * kDay);
    logs.add("c.jsonl", 30, kNow - kDay);

    const LogStats stats = summarize_logs(scan_log_entries(logs.dir.string()));
    ASSERT_EQ(stats.files, 3u);
    ASSERT_EQ(stats.bytes, 60u);
    ASSERT_EQ(stats.oldest, kNow - 3 * kDay);
    ASSERT_EQ(stats.newest, kNow - kDay);
}

TEST(scan_logs_of_a_missing_directory_is_empty) {
    const LogStats stats = summarize_logs(scan_log_entries("/nonexistent/amber/logs"));
    ASSERT_EQ(stats.files, 0u);
    ASSERT_EQ(stats.bytes, 0u);
}
