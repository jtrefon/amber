
#include "agent/process.h"

#include <fcntl.h>
#include <csignal>
#include <unistd.h>

namespace agent {

pid_t spawn_shell(const std::string& command, const std::string& cwd,
                  int& read_fd, std::string& err) {
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
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    read_fd = pipefd[0];
    return pid;
}

pid_t spawn_mcp_server(const std::string& command,
                       const std::vector<std::string>& args,
                       const std::string& cwd, int& stdin_fd, int& stdout_fd,
                       int& stderr_fd, std::string& err) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        err = "pipe failed";
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        // Child: own process group, three-way pipe wiring, chdir, direct exec.
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        setpgid(0, 0);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(command.c_str(), argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    setpgid(pid, pid);
    stdin_fd = in_pipe[1];
    stdout_fd = out_pipe[0];
    stderr_fd = err_pipe[0];
    return pid;
}

void kill_process_group(pid_t pid) {
    if (pid > 0) kill(-pid, SIGKILL);
}

} // namespace agent
