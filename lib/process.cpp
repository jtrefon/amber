
#include "agent/process.h"

#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

pid_t spawn_shell(const std::string& command, const std::string& cwd, int& read_fd,
                  std::string& err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        err = "pipe failed";
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        err = "fork failed";
        return -1;
    }
    if (pid == 0) {
        // Child: new process group, fuse stdout+stderr onto the pipe, chdir,
        // then exec the shell. Never returns.
        close(pipefd[0]);
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        // Close stdin so child processes never steal terminal input from the
        // parent's ncurses getch() — npm, react scripts, and other interactive
        // programs should not hang waiting for keyboard input.
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0)
            _exit(127);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    read_fd = pipefd[0];
    return pid;
}

namespace {

// Every end of the four pipe pairs one MCP server spawn needs. Grouped so the
// spawn reads as a sequence of steps instead of a wall of fd plumbing, and so
// each early return closes exactly one thing.
struct SpawnPipes {
    int in[2] = {-1, -1};     // parent writes, child reads (stdin)
    int out[2] = {-1, -1};    // child writes, parent reads (stdout)
    int err[2] = {-1, -1};    // child writes, parent reads (stderr)
    int status[2] = {-1, -1}; // exec status: one byte, or EOF once exec succeeds
};

void close_pair(int (&p)[2]) {
    close(p[0]);
    close(p[1]);
    p[0] = p[1] = -1;
}

// Safe on the -1 sentinels left by a failed pipe().
void close_all(SpawnPipes& p) {
    close_pair(p.in);
    close_pair(p.out);
    close_pair(p.err);
    close_pair(p.status);
}

// The exec status pipe is close-on-exec, so a successful exec closes the write
// end and the parent reads EOF. Without it a missing binary is indistinguishable
// from a server that started and exited ("closed its output"), and the operator
// gets the wrong diagnosis for the one failure they can actually fix.
bool open_spawn_pipes(SpawnPipes& p) {
    if (pipe(p.in) != 0 || pipe(p.out) != 0 || pipe(p.err) != 0 || pipe(p.status) != 0)
        return false;
    fcntl(p.status[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void report_exec_failure(int status_fd, int code) {
    const int saved = errno;
    const ssize_t wr = write(status_fd, &code, sizeof code);
    (void)wr;
    errno = saved;
}

// Wait for the child's exec: a byte means execvp failed and carries errno, EOF
// means the program is running. On failure the child is reaped and `err` names
// the program and the reason.
bool wait_for_exec(pid_t pid, SpawnPipes& p, const std::string& command, std::string& err) {
    close(p.status[1]);
    int code = 0;
    const ssize_t n = read(p.status[0], &code, sizeof code);
    close(p.status[0]);
    if (n != static_cast<ssize_t>(sizeof code))
        return true;
    int status = 0;
    waitpid(pid, &status, 0);
    err = "exec failed: " + std::string(std::strerror(code)) + " (" + command + ")";
    return false;
}

// Parent side of a spawn that got past fork(): keep the ends it reads/writes,
// drop the child's.
void close_child_ends(SpawnPipes& p) {
    close(p.in[0]);
    close(p.out[1]);
    close(p.err[1]);
}

// Parent side of a spawn whose exec failed: nothing was wired, drop what it
// still holds.
void close_parent_ends(SpawnPipes& p) {
    close(p.in[1]);
    close(p.out[0]);
    close(p.err[0]);
}

// Hand the parent its three fds once the child is running.
void hand_over_fds(SpawnPipes& p, int& stdin_fd, int& stdout_fd, int& stderr_fd) {
    stdin_fd = p.in[1];
    stdout_fd = p.out[0];
    stderr_fd = p.err[0];
}

// Child side: own process group, pipe wiring, chdir, direct exec. Never returns.
void run_child(SpawnPipes& p, const std::string& cwd, const std::string& command,
               const std::vector<std::string>& args) {
    close(p.in[1]);
    close(p.out[0]);
    close(p.err[0]);
    close(p.status[0]);
    setpgid(0, 0);
    dup2(p.in[0], STDIN_FILENO);
    dup2(p.out[1], STDOUT_FILENO);
    dup2(p.err[1], STDERR_FILENO);
    close(p.in[0]);
    close(p.out[1]);
    close(p.err[1]);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
        report_exec_failure(p.status[1], errno);
        _exit(127);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(command.c_str(), argv.data());
    report_exec_failure(p.status[1], errno);
    _exit(127);
}

} // namespace

pid_t spawn_mcp_server(const std::string& command, const std::vector<std::string>& args,
                       const std::string& cwd, int& stdin_fd, int& stdout_fd, int& stderr_fd,
                       std::string& err) {
    SpawnPipes p;
    if (!open_spawn_pipes(p)) {
        err = "pipe failed";
        close_all(p);
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        close_all(p);
        return -1;
    }
    if (pid == 0)
        run_child(p, cwd, command, args);

    close_child_ends(p);
    setpgid(pid, pid);
    if (!wait_for_exec(pid, p, command, err)) {
        close_parent_ends(p);
        return -1;
    }
    hand_over_fds(p, stdin_fd, stdout_fd, stderr_fd);
    return pid;
}

// The grace a stopped group gets before the hammer: long enough for a shell to
// run its trap and flush what it holds, short enough that an unresponsive job
// does not stall the host. Sliced so the wait ends as soon as the group is gone.
constexpr int kTermGraceSlices = 20;
constexpr int kTermGraceSliceMs = 100;

void kill_process_group(pid_t pid) {
    if (pid <= 0)
        return;
    // Ask first: SIGTERM lets the child close on its own terms. The probe is
    // kill(pid, 0), never waitpid — the JOB's reader thread owns the reap (it is
    // what records the exit code), and stealing it here would turn a graceful
    // exit into "killed, no code".
    if (kill(-pid, SIGTERM) != 0 && errno == ESRCH)
        return;
    for (int i = 0; i < kTermGraceSlices; ++i) {
        if (kill(pid, 0) != 0 && errno == ESRCH)
            return; // the group is gone; nothing to escalate
        usleep(kTermGraceSliceMs * 1000);
    }
    kill(-pid, SIGKILL);
}

} // namespace agent
