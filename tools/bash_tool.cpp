// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

namespace {
constexpr int kMaxTimeout = 3600;          // 1 hour ceiling
constexpr size_t kMaxOutput = 64 * 1024;   // 64 KiB cap

// Drain buffered output from the read end of a pipe, up to kMaxOutput bytes.
void drain_output(int fd, std::string& out) {
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        if (out.size() < kMaxOutput) out.append(buf.data(), static_cast<size_t>(n));
    }
}

// Parent-side read loop: copy pipe output until EOF or the wall-clock deadline,
// killing the child's process group on timeout. Returns true if the deadline hit.
bool run_with_timeout(int fd, pid_t pid, int timeout_s, std::string& out,
                     bool& child_done) {
    const long deadline_ms = timeout_s * 1000L;
    long elapsed_ms = 0;
    const int poll_ms = 50;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::array<char, 4096> buf{};
    while (true) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            if (out.size() < kMaxOutput)
                out.append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;  // EOF: child closed the pipe
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;

        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) { child_done = true; break; }
        if (elapsed_ms >= deadline_ms) {
            kill(-pid, SIGKILL);
            return true;
        }
        usleep(poll_ms * 1000);
        elapsed_ms += poll_ms;
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

// Child side of the fork: new process group, stdout+stderr to the pipe, chdir
// into the workspace, then exec the shell. Never returns.
[[noreturn]] void run_child(int write_fd, const std::string& cwd,
                            const std::string& command) {
    setpgid(0, 0);
    dup2(write_fd, STDOUT_FILENO);
    dup2(write_fd, STDERR_FILENO);
    close(write_fd);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
    _exit(127);
}

// Fork the shell command; on success return its pid and hand back the read end
// of the pipe via `read_fd`. Returns -1 (with r.error set) on pipe/fork failure.
pid_t spawn_command(const std::string& command, const std::string& cwd,
                    int& read_fd, ToolResult& r) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        r.ok = false;
        r.error = "pipe failed";
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        r.ok = false;
        r.error = "fork failed";
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        run_child(pipefd[1], cwd, command);
    }
    close(pipefd[1]);
    read_fd = pipefd[0];
    return pid;
}

// Assemble the combined output (with truncation notice), then either report the
// timeout or the child's exit code into `r`.
void format_result(std::string output, bool timed_out, int status, int timeout,
                   ToolResult& r) {
    bool truncated = output.size() >= kMaxOutput;
    if (truncated) output.resize(kMaxOutput);

    std::ostringstream out;
    out << output;
    if (!output.empty() && output.back() != '\n') out << '\n';
    if (truncated) out << "[output truncated at " << kMaxOutput << " bytes]\n";

    if (timed_out) {
        out << "[command timed out after " << timeout << "s and was killed]";
        r.ok = false;
        r.output = out.str();
        r.error = "timed out after " + std::to_string(timeout) + "s";
        return;
    }

    int code = WIFEXITED(status)    ? WEXITSTATUS(status)
               : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                     : -1;
    out << "[exit " << code << "]";
    r.output = out.str();
    r.ok = (code == 0);
    if (!r.ok) r.error = "command exited with status " + std::to_string(code);
}
} // namespace

// bash: run a shell command inside the workspace root and return its combined
// stdout+stderr and exit status. Args:
//   command  (string, required) the command line, run via `sh -c`
//   timeout  (int, optional)    seconds before the command is killed (default 60)
//
// Safety: this tool executes arbitrary commands, so it declares
// requires_approval() == true. The agent loop will not run it unless the host
// grants approval (a TUI confirmation dialog or a CLI opt-in). It runs with the
// working directory set to the confined workspace root, in its own process
// group so a timeout can reliably kill the whole subtree.
class BashTool : public Tool {
public:
    std::string name() const override { return "bash"; }

    std::string description() const override {
        return "Run a shell command in the workspace directory and return "
               "combined stdout/stderr and exit code. Use this for ALL file "
               "operations: cat, ls, grep, find, git, g++ builds, and tests. "
               "Write operations (rm, mv, sed -i, >) require user approval.";
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
                              "Seconds before the command is killed (default 60)"}}}
            }},
            {"required", {"command"}}
        };
    }

    bool requires_approval() const override { return false; }

    bool is_read_only() const override { return false; }

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

        format_result(std::move(output), timed_out, status, timeout, r);
        return r;
    }
};

std::unique_ptr<Tool> make_bash_tool() {
    return std::make_unique<BashTool>();
}

} // namespace agent
