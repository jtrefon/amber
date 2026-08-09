
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "agent/process.h"
#include "agent/job.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

namespace {

// Returns true for commands that can cause data loss or system damage.
// Only these trigger the approval dialog — everything else runs freely.
bool is_dangerous_shell(const std::string& cmd) {
    if (cmd.empty()) return false;
    const char* dangerous[] = {
        "rm -rf", "rm -r", "rm -f", "rm --",
        "dd", "mkfs", "fdisk", "parted",
        "git reset --hard", "git push -f", "git push --force",
        "git clean -f", "git clean -fd",
        "sudo rm", "sudo dd", "sudo mkfs",
        "chmod -R", "chown -R",
        "shutdown", "reboot", "halt", "poweroff",
        "kill -9", "pkill -9",
        "docker rm -f", "docker rmi", "docker system prune",
        "docker volume rm", "docker network rm",
        "npm uninstall", "pip uninstall",
    };
    auto matches = [&](const char* pattern) {
        if (cmd.rfind(pattern, 0) != 0) return false;
        char following = cmd[std::strlen(pattern)];
        return following == '\0' || following == ' ';
    };
    return std::any_of(std::begin(dangerous), std::end(dangerous), matches);
}

constexpr int kMaxTimeout = 3600;          // 1 hour ceiling
constexpr std::size_t kMaxOutput = std::size_t{64} * 1024;   // 64 KiB cap

// Drain buffered output from the read end of a pipe, up to kMaxOutput bytes.
void drain_output(int fd, std::string& out) {
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        if (out.size() < kMaxOutput) out.append(buf.data(), static_cast<size_t>(n));
    }
}

// Parent-side read loop: copy pipe output until EOF or the IDLE deadline,
// killing the child's process group on timeout. The budget counts time with no
// new output, so a long-running command that keeps emitting (e.g. a build)
// survives indefinitely; only silence past `timeout_s` triggers a kill.
bool run_with_timeout(int fd, pid_t pid, int timeout_s, std::string& out,
                      bool& child_done) {
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
            idle_ms = 0;  // output arrived: reset the idle budget
            continue;
        }
        if (n == 0) break;  // EOF: child closed the pipe
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;

        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) { child_done = true; break; }
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
bool parse_bash_args(const json& a, std::string& command, int& timeout,
                     ToolResult& r) {
    if (!a.contains("command") || !a["command"].is_string() ||
        a["command"].get<std::string>().empty()) {
        r.ok = false;
        r.error = "missing 'command'";
        return false;
    }
    command = a["command"].get<std::string>();
    timeout = static_cast<int>(a.value("timeout", 60));
    timeout = std::max(1, std::min(timeout, kMaxTimeout));
    return true;
}

// Milliseconds since a steady-clock start point.
long elapsed_ms(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// Fork the shell command; on success return its pid and hand back the read end
// of the pipe via `read_fd`. Returns -1 (with r.error set) on spawn failure.
pid_t spawn_command(const std::string& command, const std::string& cwd,
                     int& read_fd, ToolResult& r) {
    std::string err;
    pid_t pid = spawn_shell(command, cwd, read_fd, err);
    if (pid < 0) {
        r.ok = false;
        r.error = err;
    }
    return pid;
}

// Assemble the combined output (with truncation notice), then either report the
// timeout or the child's exit code into `r`.
void format_result(std::string output, bool timed_out, int code, int timeout,
                   long elapsed_ms, ToolResult& r) {
    bool truncated = output.size() >= kMaxOutput;
    if (truncated) output.resize(kMaxOutput);

    r.meta = {{"exit", code}, {"truncated", truncated},
              {"duration_ms", elapsed_ms}};

    std::ostringstream out;
    out << output;
    if (!output.empty() && output.back() != '\n') out << '\n';
    if (truncated) out << "[output truncated at " << kMaxOutput << " bytes]\n";

    if (timed_out) {
        out << "[command timed out after " << timeout << "s and was killed]";
        r.ok = false;
        r.error = "timed out after " + std::to_string(timeout) + "s";
        r.output = out.str();
        r.meta["timeout"] = timeout;
        return;
    }

    out << "[exit " << code << "]";
    r.output = out.str();
    r.ok = (code == 0);
    if (!r.ok) r.error = "command exited with status " + std::to_string(code);
}
} // namespace

// bash: run a shell command inside the workspace root and return its combined
// stdout+stderr and exit status. Args:
//   command  (string, required) the command line, run via `sh -c`
//   timeout  (int, optional)    seconds of NO output before the command is
//                              killed (default 60); output resets the budget, so
//                              a long-running task that keeps emitting is never cut off
//
// Safety: this tool executes arbitrary commands, so it declares
// is_dangerous_shell() == true triggers the approval gate. Benign commands
// (mkdir, touch, cp, mv, python, etc.) run freely without prompting.
// grants approval (a TUI confirmation dialog or a CLI opt-in). It runs with the
// working directory set to the confined workspace root, in its own process
// group so a timeout can reliably kill the whole subtree.
class BashTool : public Tool {
public:
    explicit BashTool(JobService* jobs = nullptr,
                      const CancellationToken& cancel_token = {})
        : jobs_(jobs), cancel_token_(cancel_token) {}

    std::string name() const noexcept override { return "bash"; }

private:
    JobService* jobs_ = nullptr;
    CancellationToken cancel_token_;


    std::string description() const noexcept override {
        return "Run a shell command in the workspace directory and return "
               "combined stdout/stderr and exit code. Handles file operations "
               "(cat, ls, grep, find, git), builds, and tests. Write "
               "operations (rm, mv, sed -i, >) require user approval.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"},
                             {"description",
                              "Shell command. Use cat/grep/ls for read-only "
                              "tasks (no approval needed). Use sed/rm/mv for "
                              "writes (requires approval). Always prefer a "
                              "single bash call over multiple separate ones."}}},
                 {"timeout", {{"type", "integer"},
                              {"description",
                               "Seconds of no output before the command is "
                               "killed (default 60); output resets the budget"}}}
            }},
            {"required", {"command"}}
        };
    }

    bool requires_approval(const json& a) const noexcept override {
        std::string cmd;
        if (a.contains("command") && a["command"].is_string())
            cmd = a["command"].get<std::string>();
        return is_dangerous_shell(cmd);
    }

    bool is_read_only() const noexcept override { return false; }

    std::string summarize(const json& a) const override {
        std::string cmd = (a.contains("command") && a["command"].is_string())
                              ? a["command"].get<std::string>() : "";
        if (cmd.size() > 200) { cmd.resize(197); cmd += "..."; }
        return "run: " + cmd;
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        std::string command;
        int timeout = 60;
        if (!parse_bash_args(a, command, timeout, r)) return r;
        const auto t0 = std::chrono::steady_clock::now();

        // When wired to a host JobService, run the command through it so the
        // process is visible in /job and on the status bar while it runs, and
        // can be killed. The result is still returned synchronously to the
        // model exactly as the direct path below does.
        if (jobs_) {
            // Idle-timeout semantics: the command is killed only after
            // `timeout` seconds with NO output. A long-running task that keeps
            // emitting output (e.g. a build) is never cut off. Enforcing it here
            // (rather than relying on the TUI's per-tick check_timeouts) keeps
            // the result identical headless and in tests.
            std::string id = jobs_->start(command, Workspace::root(),
                                           /*hard_timeout_s=*/0,
                                           /*idle_timeout_s=*/timeout);
            if (id.empty()) {
                r.ok = false;
                r.error = "spawn failed";
                return r;
            }
            bool timed_out = false;
            while (true) {
                if (cancel_token_.is_requested()) {
                    timed_out = true;
                    cancel_token_.clear();
                    break;
                }
                std::shared_ptr<Job> j = jobs_->get(id);
                if (!j) break;
                JobInfo i = j->info();
                if (j->is_done()) break;
                if (i.seconds_since_output >= timeout) {
                    timed_out = true;
                    break;
                }
                usleep(50000);
            }
            // Re-acquire the lease (a concurrent stop may have erased the
            // entry, in which case the job was killed and reaped).
            std::shared_ptr<Job> j = jobs_->get(id);
            std::string output = j ? j->output() : std::string();
            int code = j ? j->exit_code() : 0;
            bool killed = j && j->info().state == JobState::Killed;
            timed_out = timed_out || killed;
            jobs_->stop(id);  // erase: bash returns output inline, not via /job
            format_result(std::move(output), timed_out, code, timeout,
                          elapsed_ms(t0), r);
            return r;
        }

        int read_fd = -1;
        pid_t pid = spawn_command(command, Workspace::root(), read_fd, r);
        if (pid < 0) return r;
        setpgid(pid, pid);  // race-free with the child also setting it

        std::string output;
        bool child_done = false;
        bool timed_out =
            run_with_timeout(read_fd, pid, timeout, output, child_done);

        int status = 0;
        if (!child_done) {
            drain_output(read_fd, output);
            waitpid(pid, &status, 0);
        }
        close(read_fd);

        int code = WIFEXITED(status)    ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                          : -1;
        format_result(std::move(output), timed_out, code, timeout,
                      elapsed_ms(t0), r);
        return r;
    }
};

std::unique_ptr<Tool> make_bash_tool(JobService* jobs,
                                     const CancellationToken& cancel_token) {
    return std::make_unique<BashTool>(jobs, cancel_token);
}

} // namespace agent
