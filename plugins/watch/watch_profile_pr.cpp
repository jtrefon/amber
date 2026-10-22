#include "watch_profiles.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::plugins {

namespace {

using json = nlohmann::json;

struct Check {
    std::string name;
    std::string state;
    std::string link;
};

long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool sleep_cancel(int seconds, const CancellationToken& cancel) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (cancel.is_requested())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return s;
}

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

void add_journal(WatchRecord& record, const std::string& line) {
    record.journal.push_back(line);
    if (record.journal.size() > 50)
        record.journal.erase(record.journal.begin());
}

void add_attempt_evidence(WatchReport& report, const WatchRecord& record) {
    report.observed.push_back("attempt: " + std::to_string(record.attempt));
    if (!record.previous_summary.empty())
        report.observed.push_back("previous attempt: " + record.previous_summary);
}

std::vector<Check> parse_checks(const std::string& text) {
    std::vector<Check> checks;
    // gh can put a warning before the JSON; parse the array itself.
    const std::size_t first = text.find('[');
    const std::size_t last = text.rfind(']');
    const std::string body =
        (first != std::string::npos && last != std::string::npos && last > first)
            ? text.substr(first, last - first + 1)
            : text;
    try {
        const json j = json::parse(body);
        if (!j.is_array())
            return checks;
        for (const auto& item : j) {
            if (!item.is_object())
                continue;
            Check c;
            c.name = item.value("name", std::string());
            c.state = upper(item.value("state", std::string()));
            c.link = item.value("link", std::string());
            if (!c.name.empty())
                checks.push_back(std::move(c));
        }
    } catch (const std::exception&) {
        return {};
    }
    return checks;
}

bool is_pending(const std::string& state) {
    return state == "PENDING" || state == "QUEUED" || state == "IN_PROGRESS" ||
           state == "WAITING" || state.empty();
}

bool is_failure(const std::string& state) {
    return state == "FAILURE" || state == "CANCELLED" || state == "TIMED_OUT" ||
           state == "ACTION_REQUIRED" || state == "STARTUP_FAILURE";
}

std::string summary_of(const std::vector<Check>& checks) {
    std::string out;
    for (const auto& c : checks) {
        if (!out.empty())
            out += "; ";
        out += c.name + "=" + c.state;
    }
    return clip(out, 2048);
}

std::string run_id_from_link(const std::string& link) {
    const std::string marker = "/actions/runs/";
    const std::size_t at = link.find(marker);
    if (at == std::string::npos)
        return {};
    std::size_t begin = at + marker.size();
    std::size_t end = begin;
    while (end < link.size() && std::isdigit(static_cast<unsigned char>(link[end])))
        ++end;
    return end > begin ? link.substr(begin, end - begin) : std::string();
}

std::string pr_checks_command(const std::string& pr) {
    return "gh pr checks " + pr + " --json name,state,link";
}

std::string pr_meta_command(const std::string& pr) {
    return "gh pr view " + pr + " --json number,title,headRefOid,url";
}

std::string fetch_failed_logs(const WatchRecord& record, CommandRunner& runner,
                              const std::vector<Check>& checks, const CancellationToken& cancel) {
    std::string all;
    for (const auto& check : checks) {
        if (!is_failure(check.state))
            continue;
        const std::string run_id = run_id_from_link(check.link);
        std::string section = "=== " + check.name + " (" + check.state + ") ===\n";
        if (run_id.empty()) {
            section += "run link: " + check.link + "\n";
        } else {
            const auto logs =
                runner.run("gh run view " + run_id + " --log-failed", record.cwd, 60, cancel);
            section += logs.output.empty() ? "(no failed log output)\n" : logs.output;
        }
        all += section + "\n";
    }
    return clip(std::move(all), std::size_t{64} * 1024);
}

WatchReport make_report(const WatchRecord& record, WatchState state, std::string headline) {
    WatchReport report;
    report.watch_id = record.id;
    report.report_id = record.id + ":" + std::to_string(record.attempt);
    report.state = state;
    report.headline = std::move(headline);
    report.needs_attention = state != WatchState::Succeeded;
    report.confidence = "high";
    report.attempt = record.attempt;
    return report;
}

WatchReport cancelled_report(const WatchRecord& record) {
    auto report = make_report(record, WatchState::Cancelled, record.label + ": cancelled");
    report.observed.emplace_back("The watch was cancelled.");
    return report;
}

WatchReport timeout_report(const WatchRecord& record) {
    auto report = make_report(record, WatchState::TimedOut, record.label + ": timed out");
    report.observed.emplace_back("The watch reached its deadline without a terminal check state.");
    add_attempt_evidence(report, record);
    report.next_action = "Check the PR/checks directly or start a new watch.";
    return report;
}

std::string fetch_changed_files(const WatchRecord& record, CommandRunner& runner,
                                const CancellationToken& cancel);

WatchReport failure_report(WatchRecord& record, CommandRunner& runner,
                           const std::vector<Check>& checks, const ProfileHooks& hooks,
                           const CancellationToken& cancel) {
    if (cancel.is_requested())
        return cancelled_report(record);
    record.state = WatchState::Failed;
    record.ended_ms = now_ms();
    auto report =
        make_report(record, WatchState::Failed,
                    record.label + ": failed (" + std::to_string(checks.size()) + " checks)");
    report.observed.push_back("checks: " + summary_of(checks));
    add_attempt_evidence(report, record);
    for (const auto& check : checks)
        if (is_failure(check.state))
            report.observed.push_back("failed check: " + check.name + " (" + check.state + ")");
    const std::string logs = fetch_failed_logs(record, runner, checks, cancel);
    if (cancel.is_requested())
        return cancelled_report(record);
    if (!logs.empty()) {
        report.observed.push_back("failed log excerpt:\n" + clip(logs, 4096));
        report.artifact_path = hooks.artifact("failed.log", logs);
    } else {
        report.observed.emplace_back("failed log: not available from the check links");
    }
    const std::string files = fetch_changed_files(record, runner, cancel);
    if (!files.empty()) {
        report.observed.push_back("changed files:\n" + clip(files, 2048));
        const std::string path = hooks.artifact("changed-files.txt", files);
        if (!path.empty())
            report.observed.push_back("changed files artifact: " + path);
    }
    report.next_action = "Read the failed log excerpt and fix the first error.";
    return hooks.observe(record, report, cancel);
}

WatchReport success_report(WatchRecord& record, const std::vector<Check>& checks) {
    record.state = WatchState::Succeeded;
    record.ended_ms = now_ms();
    auto report =
        make_report(record, WatchState::Succeeded,
                    record.label + ": passed (" + std::to_string(checks.size()) + " checks)");
    report.observed.push_back("checks: " + summary_of(checks));
    add_attempt_evidence(report, record);
    report.confidence = "high";
    return report;
}

std::string fetch_changed_files(const WatchRecord& record, CommandRunner& runner,
                                const CancellationToken& cancel) {
    const auto result =
        runner.run("gh pr diff " + record.pr + " --name-only", record.cwd, 20, cancel);
    if (result.cancelled || result.timed_out || result.exit_code != 0)
        return {};
    return result.output;
}

bool fetch_meta(WatchRecord& record, CommandRunner& runner, const CancellationToken& cancel) {
    const auto meta = runner.run(pr_meta_command(record.pr), record.cwd, 20, cancel);
    if (meta.cancelled || meta.timed_out || meta.exit_code != 0)
        return false;
    try {
        const json j = json::parse(meta.output);
        const std::string sha = j.value("headRefOid", std::string());
        if (sha.empty())
            return false;
        if (!record.head_sha.empty() && sha != record.head_sha) {
            record.previous_summary = record.last_status;
            ++record.attempt;
            record.last_status = "attempt " + std::to_string(record.attempt);
            add_journal(record, "attempt " + std::to_string(record.attempt) + ": head " +
                                    sha.substr(0, 12));
        } else if (record.head_sha.empty()) {
            add_journal(record, "head " + sha.substr(0, 12));
        }
        record.head_sha = sha;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

WatchState state_from_checks(const std::vector<Check>& checks) {
    bool any_failure = false;
    for (const auto& c : checks)
        any_failure = any_failure || is_failure(c.state);
    return any_failure ? WatchState::Failed : WatchState::Succeeded;
}

} // namespace

bool safe_pr_argument(const std::string& value) {
    if (value.empty())
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '/' || c == ':' || c == '.' || c == '_' || c == '-' ||
               c == '#' || c == '?' || c == '=';
    });
}

std::vector<std::string> failed_check_names(const std::string& checks_json) {
    std::vector<std::string> names;
    for (const auto& check : parse_checks(checks_json))
        if (is_failure(check.state))
            names.push_back(check.name);
    return names;
}

bool checks_terminal(const std::string& checks_json) {
    const auto checks = parse_checks(checks_json);
    if (checks.empty())
        return false;
    return std::none_of(checks.begin(), checks.end(),
                        [](const Check& check) { return is_pending(check.state); });
}

struct Poll {
    enum class Kind { Continue, Cancelled, Terminal };
    Kind kind = Kind::Continue;
    std::vector<Check> checks;
};

Poll poll_pr(WatchRecord& record, CommandRunner& runner, const CancellationToken& cancel) {
    const auto result = runner.run(pr_checks_command(record.pr), record.cwd,
                                   std::min(60, record.interval_s + 10), cancel);
    if (result.cancelled)
        return {Poll::Kind::Cancelled, {}};
    // `gh pr checks` exits non-zero for pending and failed checks; the JSON is
    // still the status. Parse first, and treat the exit code as an error only
    // when there is no usable JSON.
    auto checks = parse_checks(result.output);
    if (checks.empty()) {
        record.last_status =
            result.timed_out ? "check poll timed out"
                             : "check poll failed (exit " + std::to_string(result.exit_code) + ")";
        add_journal(record, record.last_status);
        return {};
    }
    record.last_status = summary_of(checks);
    return {checks_terminal(result.output) ? Poll::Kind::Terminal : Poll::Kind::Continue,
            std::move(checks)};
}

WatchReport finalize_pr(WatchRecord& record, CommandRunner& runner, const Poll& poll,
                        const ProfileHooks& hooks, const CancellationToken& cancel) {
    if (state_from_checks(poll.checks) == WatchState::Succeeded)
        return success_report(record, poll.checks);
    return failure_report(record, runner, poll.checks, hooks, cancel);
}

WatchReport run_pr_profile(WatchRecord& record, CommandRunner& runner, const ProfileHooks& hooks,
                           const CancellationToken& cancel) {
    record.state = WatchState::Running;
    record.started_ms = now_ms();
    record.last_status = "polling PR checks";
    hooks.update(record);
    (void)fetch_meta(record, runner, cancel);

    const long deadline = record.started_ms + (static_cast<long>(record.deadline_s) * 1000);
    while (!cancel.is_requested()) {
        if (now_ms() >= deadline)
            return hooks.observe(record, timeout_report(record), cancel);
        (void)fetch_meta(record, runner, cancel);
        const Poll poll = poll_pr(record, runner, cancel);
        if (poll.kind == Poll::Kind::Cancelled)
            return cancelled_report(record);
        hooks.update(record);
        if (poll.kind == Poll::Kind::Terminal)
            return finalize_pr(record, runner, poll, hooks, cancel);
        if (!sleep_cancel(record.interval_s, cancel))
            return cancelled_report(record);
    }
    return cancelled_report(record);
}

} // namespace agent::plugins
