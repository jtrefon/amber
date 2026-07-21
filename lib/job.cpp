// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/job.h"

#include "agent/process.h"
#include "agent/workspace.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <sstream>

namespace agent {

std::unique_ptr<Job> Job::start(const std::string& id,
                                const std::string& command,
                                const std::string& cwd,
                                long hard_timeout_s, long idle_timeout_s,
                                std::string& err) {
    auto job = std::unique_ptr<Job>(new Job);
    job->id_ = id;
    job->command_ = command;
    job->cwd_ = cwd;
    job->hard_timeout_s_ = hard_timeout_s;
    job->idle_timeout_s_ = idle_timeout_s;
    job->start_ = std::chrono::steady_clock::now();
    job->last_output_ = job->start_;
    if (!job->begin(err)) return nullptr;
    return job;
}

// Non-static half of start(): spawn the process and launch the reader thread.
bool Job::begin(std::string& err) {
    int fd = -1;
    std::string spawn_err;
    pid_t pid = spawn_shell(command_, cwd_, fd, spawn_err);
    if (pid < 0) {
        err = spawn_err.empty() ? "spawn failed" : spawn_err;
        set_state(JobState::Failed);
        return false;
    }
    pid_ = pid;
    read_fd_ = fd;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    set_state(JobState::Running);
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

Job::~Job() {
    if (reader_.joinable()) {
        kill_process_group(pid_);
        reader_.join();
    }
}

JobInfo Job::info() const {
    std::scoped_lock lk(mtx_);
    JobInfo i;
    i.id = id_;
    i.command = command_;
    i.pid = static_cast<int>(pid_);
    i.state = state_;
    i.exit_code = exit_code_;
    i.bytes = output_.size();
    i.truncated = truncated_;
    i.seconds_since_start = sec_since(start_);
    i.seconds_since_output = sec_since(last_output_);
    i.remaining_idle_s =
        (idle_timeout_s_ > 0)
            ? std::max(0L, idle_timeout_s_ - sec_since(last_output_))
            : -1;
    i.remaining_hard_s =
        (hard_timeout_s_ > 0)
            ? std::max(0L, hard_timeout_s_ - sec_since(start_))
            : -1;
    return i;
}

bool Job::is_done() const {
    std::scoped_lock lk(mtx_);
    return state_ == JobState::Done || state_ == JobState::Killed ||
           state_ == JobState::Failed;
}

int Job::exit_code() const {
    std::scoped_lock lk(mtx_);
    return exit_code_;
}

std::string Job::read_delta() {
    std::scoped_lock lk(mtx_);
    if (read_cursor_ >= output_.size()) return "";
    std::string delta = output_.substr(read_cursor_);
    read_cursor_ = output_.size();
    return delta;
}

std::string Job::output() const {
    std::scoped_lock lk(mtx_);
    return output_;
}

void Job::kill() {
    bool was_running = false;
    {
        std::scoped_lock lk(mtx_);
        was_running = (state_ == JobState::Running || state_ == JobState::Starting);
    }
    if (was_running) {
        kill_process_group(pid_);
        if (reader_.joinable()) reader_.join();
        std::scoped_lock lk(mtx_);
        if (state_ != JobState::Done) {
            exit_code_ = -1;
            state_ = JobState::Killed;
        }
    } else if (reader_.joinable()) {
        reader_.join();
    }
}

bool Job::check_timeouts() {
    if (is_done()) return false;
    long idle = (idle_timeout_s_ > 0)
                    ? idle_timeout_s_ - sec_since(last_output_)
                    : 1 << 30;
    long hard = (hard_timeout_s_ > 0)
                    ? hard_timeout_s_ - sec_since(start_)
                    : 1 << 30;
    if (idle <= 0 || hard <= 0) {
        kill();
        return true;
    }
    return false;
}

void Job::reader_loop() {
    std::array<char, 4096> buf{};
    while (true) {
        ssize_t n = read(read_fd_, buf.data(), buf.size());
        if (n > 0) {
            std::scoped_lock lk(mtx_);
            if (output_.size() < kCap) {
                output_.append(buf.data(), static_cast<std::size_t>(n));
            } else if (!truncated_) {
                truncated_ = true;
            }
            last_output_ = std::chrono::steady_clock::now();
            continue;
        }
        if (n == 0) {  // EOF: child closed the pipe. Reap it (non-blocking) to
                        // capture the exit code; if it is merely a daemon that
                        // closed stdout while still running, leave exit_code_ at
                        // its default and let the state transition handle it.
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                std::scoped_lock lk(mtx_);
                if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    exit_code_ = 128 + WTERMSIG(status);
            }
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        // No data yet: has the child exited? If so, drain remaining then stop.
        int status = 0;
        pid_t w = waitpid(pid_, &status, WNOHANG);
        if (w == pid_) {
            if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
            // Drain any final buffered bytes before declaring done.
            while ((n = read(read_fd_, buf.data(), buf.size())) > 0) {
                std::scoped_lock lk(mtx_);
                if (output_.size() < kCap) output_.append(buf.data(), (std::size_t)n);
                else if (!truncated_) truncated_ = true;
            }
            break;
        }
        usleep(20000);
    }
    close(read_fd_);
    read_fd_ = -1;
    {
        std::scoped_lock lk(mtx_);
        if (state_ == JobState::Running || state_ == JobState::Starting)
            state_ = JobState::Done;
    }
}

void Job::set_state(JobState s) {
    std::scoped_lock lk(mtx_);
    state_ = s;
}

long Job::sec_since(const std::chrono::steady_clock::time_point& tp) const {
    auto d = std::chrono::steady_clock::now() - tp;
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(d).count());
}

// ---- JobService -----------------------------------------------------------

std::string JobService::start(const std::string& command,
                               const std::string& cwd, long hard_timeout_s,
                               long idle_timeout_s) {
    std::string id = std::to_string(++counter_);
    std::string err;
    std::string dir = cwd.empty() ? Workspace::root() : cwd;
    auto job = Job::start(id, command, dir, hard_timeout_s, idle_timeout_s, err);
    if (!job) return "";  // spawn failed; empty id signals error
    std::scoped_lock lk(mtx_);
    jobs_[id] = std::move(job);
    return id;
}

bool JobService::stop(const std::string& id) {
    std::unique_ptr<Job> job;
    {
        std::scoped_lock lk(mtx_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        job = std::move(it->second);
        jobs_.erase(it);
    }
    job->kill();
    return true;
}

void JobService::check_timeouts() {
    std::vector<Job*> running;
    {
        std::scoped_lock lk(mtx_);
        for (auto& kv : jobs_)
            if (!kv.second->is_done()) running.push_back(kv.second.get());
    }
    for (auto* j : running) j->check_timeouts();
}

Job* JobService::get(const std::string& id) {
    std::scoped_lock lk(mtx_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second.get();
}

std::string JobService::read_delta(const std::string& id) {
    std::scoped_lock lk(mtx_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? "" : it->second->read_delta();
}

std::string JobService::output(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? "" : it->second->output();
}

int JobService::exit_code(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? -1 : it->second->exit_code();
}

std::vector<JobInfo> JobService::list() const {
    std::vector<JobInfo> out;
    std::scoped_lock lk(mtx_);
    out.reserve(jobs_.size());
    for (auto& kv : jobs_) out.push_back(kv.second->info());
    return out;
}

int JobService::running_count() const {
    std::scoped_lock lk(mtx_);
    int n = 0;
    for (auto& kv : jobs_)
        if (!kv.second->is_done()) ++n;
    return n;
}

int JobService::min_timeout_remaining() const {
    std::scoped_lock lk(mtx_);
    int best = -1;
    for (auto& kv : jobs_) {
        const Job* j = kv.second.get();
        if (j->is_done()) continue;
        JobInfo i = j->info();
        int rem = i.remaining_hard_s;
        if (i.remaining_idle_s >= 0 &&
            (rem < 0 || i.remaining_idle_s < rem))
            rem = i.remaining_idle_s;
        if (rem >= 0 && (best < 0 || rem < best)) best = rem;
    }
    return best;
}

} // namespace agent
