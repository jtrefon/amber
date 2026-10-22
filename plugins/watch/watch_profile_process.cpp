#include "watch_profiles.h"

#include <chrono>
#include <string>
#include <vector>

namespace agent::plugins {

namespace {

long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string tail_text(const std::string& text, std::size_t cap) {
    if (text.size() <= cap)
        return text;
    return "[...truncated...]\n" + text.substr(text.size() - cap);
}

WatchReport report_for(const WatchRecord& record, WatchState state, std::string headline) {
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

} // namespace

WatchReport run_process_profile(WatchRecord& record, CommandRunner& runner,
                                const ProfileHooks& hooks, const CancellationToken& cancel) {
    record.state = WatchState::Running;
    record.started_ms = now_ms();
    record.last_status = "running";
    hooks.update(record);

    const CommandRunner::Result result =
        runner.run(record.command, record.cwd, record.deadline_s, cancel);
    record.ended_ms = now_ms();

    WatchState state = WatchState::Failed;
    if (result.cancelled)
        state = WatchState::Cancelled;
    else if (result.timed_out)
        state = WatchState::TimedOut;
    else if (result.exit_code == record.success_exit)
        state = WatchState::Succeeded;

    WatchReport report = report_for(record, state,
                                    record.label + ": " + watch_state_name(state) + " (exit " +
                                        std::to_string(result.exit_code) + ")");
    report.observed.push_back("exit code: " + std::to_string(result.exit_code));
    report.observed.push_back("duration: " + std::to_string(record.ended_ms - record.started_ms) +
                              " ms");
    const std::string tail = tail_text(result.output, 4096);
    if (!tail.empty())
        report.observed.push_back("output tail:\n" + tail);
    if (!result.output.empty())
        report.artifact_path = hooks.artifact("process.log", result.output);
    if (state != WatchState::Succeeded)
        report.next_action = "Read the output tail or open the artifact path.";
    // A clean success needs no interpretation; the failure path gets the
    // observer's explanation.
    if (state == WatchState::Succeeded)
        return report;
    return hooks.observe(record, report, cancel);
}

} // namespace agent::plugins
