// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"

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
        if (cmd.size() > 200) cmd = cmd.substr(0, 197) + "...";
        return "run: " + cmd;
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("command") || !a["command"].is_string() ||
            a["command"].get<std::string>().empty()) {
            r.ok = false; r.error = "missing 'command'"; return r;
        }
        std::string command = a["command"].get<std::string>();
        int timeout = static_cast<int>(a.value("timeout", 60));
        if (timeout < 1) timeout = 1;
        if (timeout > kMaxTimeout) timeout = kMaxTimeout;

        std::string cwd = Workspace::root();
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            r.ok = false; r.error = "pipe failed"; return r;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]); close(pipefd[1]);
            r.ok = false; r.error = "fork failed"; return r;
        }

        if (pid == 0) {
            // Child: new process group, redirect stdout+stderr to the pipe,
            // chdir into the workspace, then exec the shell.
            setpgid(0, 0);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) _exit(127); }
            execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
            _exit(127);  // exec failed
        }

        // Parent: read output with a wall-clock deadline; kill the child's
        // process group if it overruns.
        close(pipefd[1]);
        setpgid(pid, pid);  // race-free with the child also setting it

        std::string output;
        bool child_done = false;
        bool timed_out = run_with_timeout(pipefd[0], pid, timeout, output, child_done);

        // Reap the child (grab any final bytes on clean exit).
        int status = 0;
        if (!child_done) {
            drain_output(pipefd[0], output);
            waitpid(pid, &status, 0);
        }
        close(pipefd[0]);

        bool truncated = output.size() >= kMaxOutput;
        if (truncated) output.resize(kMaxOutput);

        std::ostringstream out;
        out << output;
        if (!output.empty() && output.back() != '\n') out << '\n';
        if (truncated)
            out << "[output truncated at " << kMaxOutput << " bytes]\n";

        if (timed_out) {
            out << "[command timed out after " << timeout << "s and was killed]";
            r.ok = false;
            r.output = out.str();
            r.error = "timed out after " + std::to_string(timeout) + "s";
            return r;
        }

        int code = WIFEXITED(status) ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        out << "[exit " << code << "]";
        r.output = out.str();
        r.ok = (code == 0);
        if (!r.ok) r.error = "command exited with status " + std::to_string(code);
        return r;
    }
};

std::unique_ptr<Tool> make_bash_tool() {
    return std::make_unique<BashTool>();
}

} // namespace agent
