// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_JOB_H
#define AGENT_JOB_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {

// Lifecycle of a background process managed by JobService.
enum class JobState : std::uint8_t {
    Starting,  // spawned, reader not yet confirmed alive
    Running,   // actively producing output (or at least alive)
    Done,      // exited on its own (exit_code valid)
    Killed,    // terminated by JobService (timeout or explicit stop)
    Failed,    // could not spawn
};

// Point-in-time, host-friendly snapshot of a job. All times are relative
// seconds; negative "remaining" values mean "no limit".
struct JobInfo {
    std::string id;
    std::string command;
    int pid = 0;
    JobState state = JobState::Starting;
    int exit_code = 0;
    long seconds_since_start = 0;
    long seconds_since_output = 0;
    long remaining_idle_s = -1;   // -1 = idle timeout disabled
    long remaining_hard_s = -1;   // -1 = hard timeout disabled
    std::size_t bytes = 0;
    bool truncated = false;
};

// A single background process. Owns the child pid, a reader thread that drains
// stdout+stderr into an in-memory buffer, and timeout bookkeeping. The reader
// thread is our in-process analogue of the "watch the output file" pattern:
// deltas are read directly off the pipe rather than polling a file.
class Job {
public:
    // Spawn `command` (via /bin/sh -c) in `cwd`; start a reader thread.
    // Returns nullptr on spawn failure (with `err` set).
    static std::unique_ptr<Job> start(const std::string& id,
                                      const std::string& command,
                                      const std::string& cwd,
                                      long hard_timeout_s, long idle_timeout_s,
                                      std::string& err);

    ~Job();

    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    const std::string& id() const { return id_; }
    JobInfo info() const;
    bool is_done() const;
    int exit_code() const;   // valid once the job has exited/killed

    // Return output produced since the previous call (and only that), advancing
    // an internal cursor. Cheap; safe to call frequently from the agent loop.
    std::string read_delta();
    std::string output() const;

    // Kill the process group; joins the reader. Idempotent.
    void kill();

    // If running and past its idle or hard deadline, kill it and record why.
    // Returns true if this call ended the job.
    bool check_timeouts();

private:
    Job() = default;
    bool begin(std::string& err);
    void reader_loop();
    void set_state(JobState s);
    long sec_since(const std::chrono::steady_clock::time_point& tp) const;

    std::string id_;
    std::string command_;
    std::string cwd_;
    pid_t pid_ = 0;
    int read_fd_ = -1;
    long hard_timeout_s_ = 0;
    long idle_timeout_s_ = 0;

    mutable std::mutex mtx_;
    JobState state_ = JobState::Starting;
    std::string output_;        // protected by mtx_
    std::size_t read_cursor_ = 0;
    bool truncated_ = false;
    int exit_code_ = 0;
    static constexpr std::size_t kCap = 1 << 20;  // 1 MiB output cap
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_output_;

    std::thread reader_;
};

// Host-owned registry of background jobs. Thread-safe; the TUI and the
// process_* tools share one instance so a job started by the model is visible
// (and killable) from the UI and vice versa.
class JobService {
public:
    // Start a background job. `cwd` defaults to the confined workspace root.
    // `idle_timeout_s`/`hard_timeout_s` are seconds (0 disables that limit).
    std::string start(const std::string& command, const std::string& cwd = "",
                      long hard_timeout_s = 600, long idle_timeout_s = 30);

    // Kill a job by id; returns true if it existed.
    bool stop(const std::string& id);

    // Scan running jobs and end any past their deadline.
    void check_timeouts();

    Job* get(const std::string& id);
    std::string read_delta(const std::string& id);
    std::string output(const std::string& id) const;
    int exit_code(const std::string& id) const;
    std::vector<JobInfo> list() const;
    int running_count() const;

    // Seconds until the next running job hits an idle/hard deadline, or -1 when
    // there are no running jobs or no deadline is armed.
    int min_timeout_remaining() const;

private:
    mutable std::mutex mtx_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;
    long counter_ = 0;
};

} // namespace agent

#endif // AGENT_JOB_H
