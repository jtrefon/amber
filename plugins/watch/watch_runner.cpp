#include "watch_runner.h"

#include "agent/workspace.h"

#include <chrono>
#include <string>
#include <thread>

namespace agent::plugins {

namespace {

constexpr std::size_t kOutputCap = std::size_t{1024} * 1024;
constexpr int kPollMs = 100;

std::string capped(std::string text) {
    if (text.size() > kOutputCap)
        text.resize(kOutputCap);
    return text;
}

std::string shell_quote(const std::string& text) {
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

// A window's worktree can sit outside the process-wide project root, which
// JobService's initial-cwd confinement rejects. Run from the project root and
// enter the worktree in the shell; the watch itself was approved with its spec.
std::pair<std::string, std::string> command_and_cwd(const std::string& command,
                                                    const std::string& cwd) {
    if (cwd.empty())
        return {command, cwd};
    std::string resolved;
    std::string error;
    if (Workspace::confine(cwd, resolved, error))
        return {command, cwd};
    return {"cd " + shell_quote(cwd) + " && " + command, std::string()};
}

} // namespace

CommandRunner::Result JobCommandRunner::run(const std::string& command, const std::string& cwd,
                                            int timeout_s, const CancellationToken& cancel) {
    Result result;
    const auto [actual_command, actual_cwd] = command_and_cwd(command, cwd);
    const std::string id =
        jobs_.start(actual_command, actual_cwd, /*hard_timeout_s=*/0, /*idle_timeout_s=*/0);
    if (id.empty()) {
        result.output = "spawn failed";
        return result;
    }
    auto job = jobs_.get(id);
    if (!job)
        return result;

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(timeout_s > 0 ? timeout_s : 1);
    while (true) {
        if (cancel.is_requested()) {
            result.output = capped(job->output());
            jobs_.stop(id);
            result.cancelled = true;
            return result;
        }
        if (job->is_done()) {
            result.output = capped(job->output());
            result.exit_code = job->exit_code();
            jobs_.stop(id);
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.output = capped(job->output());
            jobs_.stop(id);
            result.timed_out = true;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

} // namespace agent::plugins
