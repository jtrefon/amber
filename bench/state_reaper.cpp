#include "bench/state_reaper.h"

#include <chrono>
#include <system_error>

namespace bench {
namespace {

namespace fs = std::filesystem;

bool has_scenario_prefix(const fs::path& p) {
    return p.filename().string().rfind(kScenarioStatePrefix, 0) == 0;
}

bool is_scenario_dir(const fs::path& p, std::error_code& ec) {
    const fs::file_status status = fs::symlink_status(p, ec);
    if (ec)
        return false;
    return fs::is_directory(status) && !fs::is_symlink(status);
}

bool is_older_than(const fs::path& p, int days, std::error_code& ec) {
    const fs::file_time_type modified = fs::last_write_time(p, ec);
    if (ec)
        return false;
    return fs::file_time_type::clock::now() - modified > std::chrono::hours(24LL * days);
}

std::uintmax_t directory_size(const fs::path& p, std::error_code& ec) {
    std::uintmax_t total = 0;
    for (fs::recursive_directory_iterator it(p, ec), end; it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (it->is_regular_file(file_ec))
            total += it->file_size(file_ec);
        if (file_ec) {
            ec = file_ec;
            break;
        }
    }
    return total;
}

std::string describe(const char* action, const fs::path& p, const std::error_code& ec) {
    return std::string(action) + " " + p.string() + ": " + ec.message();
}

enum class EntryKind { Ignore, Young, Candidate };

struct Selection {
    EntryKind kind = EntryKind::Ignore;
    std::uintmax_t bytes = 0;
    std::error_code size_error;
};

// Classify one directory entry. `ec` reports a stat/age failure the caller must
// surface; a size failure still yields a candidate, because an unreadable size
// must not stop the removal.
Selection select_entry(const fs::path& path, int days, std::error_code& ec) {
    if (!has_scenario_prefix(path))
        return {};
    if (!is_scenario_dir(path, ec) || ec)
        return {};
    if (!is_older_than(path, days, ec))
        return ec ? Selection{} : Selection{EntryKind::Young};
    Selection selection{EntryKind::Candidate, directory_size(path, ec)};
    selection.size_error = ec;
    ec.clear();
    return selection;
}

// Fold one candidate into the report, removing it first when asked. A size
// failure is reported but does not block the removal.
void record_candidate(ReapReport& report, const fs::path& path, const Selection& selection,
                      bool apply) {
    if (selection.size_error)
        report.errors.push_back(describe("cannot size", path, selection.size_error));
    if (apply) {
        std::error_code remove_ec;
        fs::remove_all(path, remove_ec);
        if (remove_ec) {
            report.errors.push_back(describe("cannot remove", path, remove_ec));
            return;
        }
    }
    report.entries.push_back({path, selection.bytes});
    report.bytes += selection.bytes;
}

} // namespace

ReapReport reap_scenario_state(const fs::path& projects_root, const ReapOptions& options) {
    ReapReport report;
    if (options.older_than_days < 1) {
        report.errors.emplace_back("older_than_days must be at least 1");
        return report;
    }
    std::error_code ec;
    fs::directory_iterator it(projects_root, ec);
    if (ec) {
        if (ec != std::errc::no_such_file_or_directory)
            report.errors.push_back(describe("cannot scan", projects_root, ec));
        return report;
    }
    for (const auto& entry : it) {
        const fs::path path = entry.path();
        std::error_code entry_ec;
        const Selection selection = select_entry(path, options.older_than_days, entry_ec);
        if (entry_ec) {
            report.errors.push_back(describe("cannot inspect", path, entry_ec));
            continue;
        }
        if (selection.kind == EntryKind::Ignore)
            continue;
        if (selection.kind == EntryKind::Young) {
            ++report.skipped_young;
            continue;
        }
        record_candidate(report, path, selection, options.apply);
    }
    return report;
}

} // namespace bench
