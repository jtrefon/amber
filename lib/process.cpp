
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

// Close every end of the three pipe pairs; safe on the -1 sentinels left by a
// failed pipe().
void close_all(int (&in_pipe)[2], int (&out_pipe)[2], int (&err_pipe)[2]) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
}

// Exec status pipe: close-on-exec, so a successful exec closes the write end and
// the parent reads EOF. Without it a missing binary is indistinguishable from a
// server that started and exited ("closed its output"), and the operator gets
// the wrong diagnosis for the one failure they can actually fix.
void make_exec_status_pipe(int (&fds)[2]) {
    if (pipe(fds) != 0) {
        fds[0] = fds[1] = -1;
        return;
    }
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
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
bool wait_for_exec(pid_t pid, int (&status_pipe)[2], const std::string& command, std::string& err) {
    close(status_pipe[1]);
    int code = 0;
    const ssize_t n = read(status_pipe[0], &code, sizeof code);
    close(status_pipe[0]);
    if (n != static_cast<ssize_t>(sizeof code))
        return true;
    int status = 0;
    waitpid(pid, &status, 0);
    err = "exec failed: " + std::string(std::strerror(code)) + " (" + command + ")";
    return false;
}

// Child side: own process group, three-way pipe wiring, chdir, direct exec.
// Never returns.
void run_child(int (&in_pipe)[2], int (&out_pipe)[2], int (&err_pipe)[2], int (&status_pipe)[2],
               const std::string& cwd, const std::string& command,
               const std::vector<std::string>& args) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);
    close(status_pipe[0]);
    setpgid(0, 0);
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
        report_exec_failure(status_pipe[1], errno);
        _exit(127);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(command.c_str(), argv.data());
    report_exec_failure(status_pipe[1], errno);
    _exit(127);
}

} // namespace

pid_t spawn_mcp_server(const std::string& command, const std::vector<std::string>& args,
                       const std::string& cwd, int& stdin_fd, int& stdout_fd, int& stderr_fd,
                       std::string& err) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int status_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        err = "pipe failed";
        close_all(in_pipe, out_pipe, err_pipe);
        return -1;
    }
    make_exec_status_pipe(status_pipe);
    if (status_pipe[0] < 0) {
        err = "pipe failed";
        close_all(in_pipe, out_pipe, err_pipe);
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        close_all(in_pipe, out_pipe, err_pipe);
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }
    if (pid == 0)
        run_child(in_pipe, out_pipe, err_pipe, status_pipe, cwd, command, args);

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    setpgid(pid, pid);
    if (!wait_for_exec(pid, status_pipe, command, err)) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        return -1;
    }
    stdin_fd = in_pipe[1];
    stdout_fd = out_pipe[0];
    stderr_fd = err_pipe[0];
    return pid;
}

void kill_process_group(pid_t pid) {
    if (pid > 0)
        kill(-pid, SIGKILL);
}

} // namespace agent
