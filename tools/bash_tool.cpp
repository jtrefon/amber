
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "agent/process.h"
#include "agent/job.h"
#include "agent/shell_classify.h" // approval classification for the gate
#include "agent/run_scope.h"      // run_cancelled: the calling agent's token
#include "agent/json_read.h"
#include "agent/task.h"
#include "agent/task_job.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

namespace {

constexpr int kMaxTimeout = 3600;                          // 1 hour ceiling
constexpr std::size_t kMaxOutput = std::size_t{64} * 1024; // 64 KiB cap

// Drain buffered output from the read end of a pipe, up to kMaxOutput bytes.
void drain_output(int fd, std::string& out) {
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        if (out.size() < kMaxOutput)
            out.append(buf.data(), static_cast<size_t>(n));
    }
}

// The two backends one bash call may touch: the job service that owns the
// process and the inventory the inactive handoff registers it in.
struct JobBackends {
    JobService* jobs = nullptr;
    TaskService* tasks = nullptr;
};

// A job may finish before or after the bash tool decides to hand it back as a
// task. The relay stores the one terminal snapshot and invokes the callback
// exactly once, whether the callback was installed first or last.
struct JobCompletionRelay {
    std::mutex mtx;
    std::optional<JobInfo> info;
    std::function<void(const JobInfo&)> callback;
    bool delivered = false;

    void on_job(const JobInfo& snapshot) {
        std::function<void(const JobInfo&)> ready;
        {
            std::scoped_lock lk(mtx);
            info = snapshot;
            if (callback && !delivered) {
                delivered = true;
                ready = callback;
            }
        }
        if (ready)
            ready(snapshot);
    }

    void set_callback(std::function<void(const JobInfo&)> cb) {
        std::optional<JobInfo> ready;
        {
            std::scoped_lock lk(mtx);
            if (delivered)
                return;
            callback = std::move(cb);
            if (info) {
                delivered = true;
                ready = info;
            }
        }
        if (ready)
            callback(*ready);
    }
};

// Parent-side read loop for hosts without a job service: copy pipe output until
// EOF or the child exits, and kill the group after `timeout_s` of NO output.
// There is no wall-clock budget (an emitting command is never cut off), and the
// budget is inactivity only — but with no registry there is nobody to hand the
// process back to, so silence is the one case that ends it here. Hosts with a
// job service return control to the agent instead (see run_via_jobs).
bool run_with_timeout(int fd, pid_t pid, int timeout_s, std::string& out, bool& child_done) {
    const long deadline_ms = timeout_s * 1000L;
    long idle_ms = 0;
    const int poll_ms = 50;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::array<char, 4096> buf{};
    while (true) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            if (out.size() < kMaxOutput)
                out.append(buf.data(), static_cast<size_t>(n));
            idle_ms = 0; // output arrived: reset the idle budget
            continue;
        }
        if (n == 0)
            break; // EOF: child closed the pipe
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            break;

        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            child_done = true;
            break;
        }
        if (idle_ms >= deadline_ms) {
            kill_process_group(pid);
            return true;
        }
        usleep(poll_ms * 1000);
        idle_ms += poll_ms;
    }
    return false;
}

// Extract and validate the command; clamp timeout to [1, kMaxTimeout]. Returns
// false (with r.error set) when no command was supplied.
bool parse_bash_args(const json& a, std::string& command, int& timeout, ToolResult& r) {
    if (!a.contains("command") || !a["command"].is_string() ||
        a["command"].get<std::string>().empty()) {
        r.ok = false;
        r.error = "missing 'command'";
        return false;
    }
    command = a["command"].get<std::string>();
    timeout = static_cast<int>(read_int(a, "timeout", 60));
    timeout = std::max(1, std::min(timeout, kMaxTimeout));
    return true;
}

// Milliseconds since a steady-clock start point.
long elapsed_ms(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

// Fork the shell command; on success return its pid and hand back the read end
// of the pipe via `read_fd`. Returns -1 (with r.error set) on spawn failure.
pid_t spawn_command(const std::string& command, const std::string& cwd, int& read_fd,
                    ToolResult& r) {
    std::string err;
    pid_t pid = spawn_shell(command, cwd, read_fd, err);
    if (pid < 0) {
        r.ok = false;
        r.error = err;
    }
    return pid;
}

// How a supervised command stopped.
enum class StopKind { Finished, Inactive, TimedOut, Cancelled };

// What the tool knows when it stops watching: the outcome, the exit status (a
// finished command), the silence that ended the wait, and the job to hand back.
struct Stop {
    StopKind kind = StopKind::Finished;
    int code = 0;
    long idle_s = 0;
    long elapsed_ms = 0;
    std::size_t bytes = 0;
    std::string job_id;
    std::string task_id;
};

// An inactive command is handed back, not killed: name the task, the silence
// and the ways forward, so the decision stays the agent's.
void format_inactive(std::ostringstream& out, const Stop& s, ToolResult& r) {
    out << "[inactive " << s.idle_s << "s] still running";
    if (!s.task_id.empty())
        out << " — task " << s.task_id;
    else if (!s.job_id.empty())
        out << " — job " << s.job_id;
    out << ", " << s.elapsed_ms / 1000 << "s elapsed, " << s.bytes
        << " bytes of output so far. Nothing was killed.\n";
    if (!s.task_id.empty()) {
        out << "Decide: tasks output " << s.task_id << " to read it, tasks wait " << s.task_id
            << " timeout_s=" << s.idle_s << " to wait, or tasks cancel " << s.task_id
            << " to end it.";
        r.meta["task"] = s.task_id;
    } else {
        out << "No task inventory is wired on this host; the process remains job " << s.job_id
            << ".";
    }
    r.ok = true;
    r.output = out.str();
    r.meta["stop"] = "inactive";
    r.meta["idle_s"] = s.idle_s;
    r.meta["duration_ms"] = s.elapsed_ms;
    if (!s.job_id.empty())
        r.meta["job"] = s.job_id;
}

// A command that was ended rather than handed back: say how, and after how long
// it had been inactive, so the next decision is informed.
void format_kill_notice(std::ostringstream& out, const Stop& s, ToolResult& r) {
    if (s.kind == StopKind::TimedOut)
        out << "[command timed out after " << s.idle_s << "s of no output and was killed]";
    else
        out << "[command cancelled and killed after " << s.elapsed_ms / 1000 << "s]";
    r.ok = false;
    r.error = s.kind == StopKind::TimedOut ? "timed out after " + std::to_string(s.idle_s) + "s"
                                           : "cancelled";
    r.output = out.str();
    r.meta["timeout"] = true;
}

// Assemble the combined output (with truncation notice), then report the
// outcome: a finished command by its exit code, an inactive one by handing the
// decision back to the agent, an ended one by the notice above.
void format_result(std::string output, const Stop& s, ToolResult& r) {
    const bool truncated = output.size() >= kMaxOutput;
    if (truncated)
        output.resize(kMaxOutput);

    std::ostringstream out;
    out << output;
    if (!output.empty() && output.back() != '\n')
        out << '\n';
    if (truncated)
        out << "[output truncated at " << kMaxOutput << " bytes]\n";

    if (s.kind == StopKind::Inactive) {
        format_inactive(out, s, r);
        return;
    }

    r.meta = {{"exit", s.code}, {"truncated", truncated}, {"duration_ms", s.elapsed_ms}};

    if (s.kind != StopKind::Finished) {
        format_kill_notice(out, s, r);
        return;
    }

    out << "[exit " << s.code << "]";
    r.output = out.str();
    r.ok = (s.code == 0);
    if (!r.ok)
        r.error = "command exited with status " + std::to_string(s.code);
}

// Wait until the job finishes, is cancelled, or has been silent for `idle_s`.
// Silence is not a kill: it returns control to the agent, which decides what
// happens next (see format_result's StopKind::Inactive).
StopKind wait_for_job(JobService* jobs, const CancellationToken& cancel_token,
                      const std::string& id, int idle_s) {
    while (true) {
        if (run_cancelled(cancel_token))
            return StopKind::Cancelled;
        std::shared_ptr<Job> j = jobs->get(id);
        if (!j || j->is_done())
            return StopKind::Finished;
        if (j->info().seconds_since_output >= idle_s)
            return StopKind::Inactive;
        usleep(50000);
    }
}

// Run through the host JobService so the process is visible in /job and on the
// status bar while it runs, and can be killed. The result is still returned
// synchronously to the model, exactly as the direct path does.
// Register an inactive job as a task and arrange for its completion to finish
// the record. An empty task id with an error means the inventory refused the
// record; the caller then stops the job rather than leaving it invisible.
std::string register_inactive_task(const JobBackends& backends, const std::string& job_id,
                                   const std::string& command,
                                   const std::shared_ptr<JobCompletionRelay>& relay,
                                   std::string& error) {
    if (!backends.tasks)
        return {};
    TaskRecord record;
    record.kind = TaskKind::Command;
    record.owner = task_owner_from_scope();
    record.label = command.size() > 80 ? command.substr(0, 77) + "..." : command;
    record.command = command;
    record.cwd = Workspace::workspace_root();
    record.notify = TaskNotify::Auto;
    std::string task_id = backends.tasks->create(std::move(record), &error);
    if (task_id.empty())
        return {};
    backends.tasks->started(task_id, 0);
    backends.tasks->bind_backend(task_id, job_id);
    backends.tasks->attach_cancel(
        task_id,
        [jobs = backends.jobs, job_id](const CancellationToken&) { return jobs->stop(job_id); });
    const std::string task = task_id;
    relay->set_callback([tasks = backends.tasks, task](const JobInfo& info) {
        tasks->finish(task, task_state_from_job(info), task_result_from_job(info),
                      task_error_from_job(info));
    });
    return task_id;
}

void run_via_jobs(const JobBackends& backends, const CancellationToken& cancel_token,
                  const std::string& command, int timeout,
                  const std::chrono::steady_clock::time_point& t0, ToolResult& r) {
    auto relay = std::make_shared<JobCompletionRelay>();
    // Neither budget is a kill: 0 disables the service's own timers, so it
    // watches and reports and the agent stays in charge of ending the process.
    const std::string id = backends.jobs->start(
        command, Workspace::workspace_root(), /*hard_timeout_s=*/0, /*idle_timeout_s=*/0, {},
        [relay](const JobInfo& info) { relay->on_job(info); });
    if (id.empty()) {
        r.ok = false;
        r.error = "spawn failed";
        return;
    }
    const StopKind kind = wait_for_job(backends.jobs, cancel_token, id, timeout);
    // Re-acquire the lease (a concurrent stop may have erased the entry).
    std::shared_ptr<Job> j = backends.jobs->get(id);
    const long idle_s = j ? j->info().seconds_since_output : 0;
    std::string output = j ? j->output() : std::string();
    const int code = j ? j->exit_code() : 0;
    Stop stop;
    stop.kind = kind;
    stop.code = code;
    stop.idle_s = idle_s;
    stop.elapsed_ms = elapsed_ms(t0);
    stop.bytes = output.size();
    if (kind == StopKind::Inactive) {
        stop.job_id = id;
        std::string task_error;
        stop.task_id = register_inactive_task(backends, id, command, relay, task_error);
        if (stop.task_id.empty() && !task_error.empty()) {
            backends.jobs->stop(id);
            r.ok = false;
            r.error = task_error;
            r.output = "the task inventory is full; the command was stopped";
            return;
        }
    } else {
        backends.jobs->stop(id); // erase: bash returns output inline, not via the task list
    }
    format_result(std::move(output), stop, r);
}

// Exit code for a waitpid status, or -1 when the child neither exited nor was
// signalled.
int wait_status_code(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

void run_direct(const std::string& command, int timeout,
                const std::chrono::steady_clock::time_point& t0, ToolResult& r) {
    int read_fd = -1;
    const pid_t pid = spawn_command(command, Workspace::workspace_root(), read_fd, r);
    if (pid < 0)
        return;
    setpgid(pid, pid); // race-free with the child also setting it

    std::string output;
    bool child_done = false;
    const bool timed_out = run_with_timeout(read_fd, pid, timeout, output, child_done);

    int status = 0;
    if (!child_done) {
        drain_output(read_fd, output);
        waitpid(pid, &status, 0);
    }
    close(read_fd);

    Stop stop;
    stop.kind = timed_out ? StopKind::TimedOut : StopKind::Finished;
    stop.code = wait_status_code(status);
    stop.idle_s = timeout;
    stop.elapsed_ms = elapsed_ms(t0);
    stop.bytes = output.size();
    format_result(std::move(output), stop, r);
}

} // namespace

// bash: run a shell command inside the workspace root and return its combined
// stdout+stderr and exit status. Args:
//   command  (string, required) the command line, run via `sh -c`
//   timeout  (int, optional)    seconds of NO output after which control
//                              returns to the agent (default 60). Nothing is
//                              killed: the command keeps running as a job and
//                              the agent decides to wait again, wait a
//                              different amount, or stop it. Hosts without a
//                              job service have no way to hand the process
//                              back, so there silence ends it.
//
// Safety: this tool executes arbitrary commands, so commands outside the
// read-only allowlist trigger the approval gate. Benign read-only commands
// (ls, cat, grep, git status, ...) run freely; write/destructive commands
// prompt first. It runs with the working directory set to the confined
// workspace root, in its own process group so the whole subtree can be ended
// when the agent asks.
class BashTool : public Tool {
public:
    explicit BashTool(JobService* jobs = nullptr, const CancellationToken& cancel_token = {},
                      TaskService* tasks = nullptr)
        : jobs_(jobs), cancel_token_(cancel_token), tasks_(tasks) {}

    std::string name() const noexcept override { return "bash"; }

private:
    JobService* jobs_ = nullptr;
    CancellationToken cancel_token_;
    TaskService* tasks_ = nullptr;

    std::string description() const noexcept override {
        return "Run a shell command in the workspace directory and return "
               "combined stdout/stderr and exit code. Handles file operations "
               "(cat, ls, grep, find, git status), builds, and tests. "
               "Destructive or state-changing operations (rm, mv, sed -i, "
               "write redirects) require user approval.";
    }

    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"command",
                   {{"type", "string"},
                    {"description", "Shell command. Use cat/grep/ls/git status for "
                                    "read-only tasks (no approval needed). "
                                    "Destructive commands (rm, mv, sed -i, writes) "
                                    "require approval. Always prefer a single bash "
                                    "call over multiple separate ones."}}},
                  {"timeout",
                   {{"type", "integer"},
                    {"description", "Seconds of no output after which control "
                                    "returns to you (default 60). Nothing is "
                                    "killed: the command keeps running as a "
                                    "task you can inspect with tasks."}}}}},
                {"required", {"command"}}};
    }

    bool requires_approval(const json& a) const noexcept override {
        std::string cmd;
        if (a.contains("command") && a["command"].is_string())
            cmd = a["command"].get<std::string>();
        // A command must be PROVABLY read-only to run without approval;
        // anything else (writes, destructive commands, escapes) is gated.
        return classify_shell(cmd, Workspace::workspace_root()).effect != ShellEffect::ReadOnly;
    }

    bool is_read_only() const noexcept override { return false; }

    std::string summarize(const json& a) const override {
        std::string cmd = (a.contains("command") && a["command"].is_string())
                              ? a["command"].get<std::string>()
                              : "";
        if (cmd.size() > 200) {
            cmd.resize(197);
            cmd += "...";
        }
        return "run: " + cmd;
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        std::string command;
        int timeout = 60;
        if (!parse_bash_args(a, command, timeout, r))
            return r;
        const auto t0 = std::chrono::steady_clock::now();

        // When wired to a host JobService, run the command through it so the
        // process is visible in /job and on the status bar while it runs, and
        // can be killed; otherwise spawn and supervise it directly.
        if (jobs_)
            run_via_jobs({jobs_, tasks_}, cancel_token_, command, timeout, t0, r);
        else
            run_direct(command, timeout, t0, r);
        return r;
    }
};

std::unique_ptr<Tool> make_bash_tool(JobService* jobs, const CancellationToken& cancel_token,
                                     TaskService* tasks) {
    return std::make_unique<BashTool>(jobs, cancel_token, tasks);
}

} // namespace agent
