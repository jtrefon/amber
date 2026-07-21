// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    read_fd = pipefd[0];
    return pid;
}

void kill_process_group(pid_t pid) {
    if (pid > 0) kill(-pid, SIGKILL);
}

} // namespace agent
