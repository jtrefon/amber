#include "watch_profiles.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace agent::plugins {

namespace {

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

std::string quote_shell(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::uint64_t body_hash(const std::string& body) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : body) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hash_text(std::uint64_t hash) {
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xf];
        hash >>= 4;
    }
    return out;
}

std::string first_difference(const std::string& before, const std::string& after) {
    const std::size_t n = std::min(before.size(), after.size());
    std::size_t i = 0;
    while (i < n && before[i] == after[i])
        ++i;
    const std::size_t line =
        1 + static_cast<std::size_t>(std::count(before.begin(), before.begin() + i, '\n'));
    return "first difference at byte " + std::to_string(i) + " (line " + std::to_string(line) + ")";
}

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

WatchReport make_report(const WatchRecord& record, WatchState state, std::string headline,
                        bool attention) {
    WatchReport report;
    report.watch_id = record.id;
    report.report_id = record.id + ":" + std::to_string(record.attempt);
    report.state = state;
    report.headline = std::move(headline);
    report.needs_attention = attention;
    report.confidence = "high";
    report.attempt = record.attempt;
    return report;
}

WatchReport cancelled(const WatchRecord& record) {
    auto report = make_report(record, WatchState::Cancelled, record.label + ": cancelled", false);
    report.observed.emplace_back("The watch was cancelled.");
    return report;
}

} // namespace

bool safe_url(const std::string& value) {
    if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0)
        return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c > 0x20 && c != 0x7f; });
}

struct UrlPoll {
    enum class Kind { Body, Cancelled, Error };
    Kind kind = Kind::Error;
    std::string body;
    std::string hash;
    std::string error;
};

UrlPoll poll_url(WatchRecord& record, CommandRunner& runner, const CancellationToken& cancel) {
    const int wait = std::min(60, record.interval_s + 10);
    const std::string command =
        "curl -fsS --max-time " + std::to_string(wait) + " " + quote_shell(record.url);
    const auto result = runner.run(command, record.cwd, wait, cancel);
    if (result.cancelled)
        return {UrlPoll::Kind::Cancelled, {}, {}, {}};
    if (result.timed_out || result.exit_code != 0) {
        UrlPoll out;
        out.kind = UrlPoll::Kind::Error;
        out.error = result.timed_out
                        ? "curl timed out (exit " + std::to_string(result.exit_code) + ")"
                        : "curl exited " + std::to_string(result.exit_code);
        record.last_status = out.error;
        record.journal.push_back(out.error);
        if (record.journal.size() > 50)
            record.journal.erase(record.journal.begin());
        return out;
    }
    return {UrlPoll::Kind::Body, result.output, hash_text(body_hash(result.output)), {}};
}

WatchReport url_timeout(const WatchRecord& record, const std::string& baseline_hash,
                        const std::string& last_error, const ProfileHooks& hooks,
                        const CancellationToken& cancel) {
    const bool failed = !last_error.empty();
    auto report = make_report(record, failed ? WatchState::Failed : WatchState::TimedOut,
                              record.label + (failed ? ": fetch failed" : ": no change"), failed);
    report.observed.push_back("last status: " + record.last_status);
    if (!baseline_hash.empty())
        report.observed.push_back("baseline hash: " + baseline_hash);
    if (failed)
        report.observed.push_back("last error: " + last_error);
    else
        report.observed.emplace_back("the body did not change before the deadline");
    return hooks.observe(record, report, cancel);
}

WatchReport url_changed(WatchRecord& record, const std::string& baseline_hash,
                        const std::string& baseline_body, const UrlPoll& poll,
                        const ProfileHooks& hooks, const CancellationToken& cancel) {
    record.state = WatchState::Succeeded;
    record.ended_ms = now_ms();
    record.last_status = "changed " + poll.hash;
    auto report = make_report(record, WatchState::Succeeded, record.label + ": body changed", true);
    report.observed.push_back("baseline hash: " + baseline_hash);
    report.observed.push_back("new hash: " + poll.hash);
    report.observed.push_back(first_difference(baseline_body, poll.body));
    if (!poll.body.empty())
        report.observed.push_back("new body tail:\n" + clip(poll.body, 4096));
    report.artifact_path = hooks.artifact("url-change.body", poll.body);
    report.next_action = "Inspect the new body or ask the observer what changed.";
    return hooks.observe(record, report, cancel);
}

void url_set_baseline(WatchRecord& record, const UrlPoll& poll, const ProfileHooks& hooks) {
    record.last_status = "baseline " + poll.hash;
    record.journal.push_back("baseline " + poll.hash);
    (void)hooks.artifact("url-baseline.body", poll.body);
    hooks.update(record);
}

void url_unchanged(WatchRecord& record, const std::string& hash, const ProfileHooks& hooks) {
    record.last_status = "unchanged " + hash;
    hooks.update(record);
}

WatchReport run_url_profile(WatchRecord& record, CommandRunner& runner, const ProfileHooks& hooks,
                            const CancellationToken& cancel) {
    record.state = WatchState::Running;
    record.started_ms = now_ms();
    record.last_status = "fetching baseline";
    hooks.update(record);

    const long deadline = record.started_ms + (static_cast<long>(record.deadline_s) * 1000);
    std::string baseline_hash;
    std::string baseline_body;
    std::string last_error;
    bool have_baseline = false;

    while (!cancel.is_requested()) {
        if (now_ms() >= deadline)
            return url_timeout(record, baseline_hash, last_error, hooks, cancel);
        const UrlPoll poll = poll_url(record, runner, cancel);
        if (poll.kind == UrlPoll::Kind::Cancelled)
            return cancelled(record);
        if (poll.kind == UrlPoll::Kind::Error) {
            last_error = poll.error;
            hooks.update(record);
            if (!sleep_cancel(record.interval_s, cancel))
                return cancelled(record);
            continue;
        }
        last_error.clear();
        if (!have_baseline) {
            baseline_hash = poll.hash;
            baseline_body = poll.body;
            have_baseline = true;
            url_set_baseline(record, poll, hooks);
        } else if (poll.hash == baseline_hash) {
            url_unchanged(record, poll.hash, hooks);
        } else {
            return url_changed(record, baseline_hash, baseline_body, poll, hooks, cancel);
        }
        if (!sleep_cancel(record.interval_s, cancel))
            return cancelled(record);
    }
    return cancelled(record);
}

} // namespace agent::plugins
