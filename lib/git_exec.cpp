#include "agent/git_exec.h"

#include <cerrno>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace agent {

namespace {

// The parent environment with `env` applied. Built before spawning: setenv in a
// forked child of a multithreaded process is not async-signal-safe.
std::vector<std::string>
child_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> out;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::size_t equals = text.find('=');
        const std::string key = text.substr(0, equals);
        bool overridden = false;
        for (const auto& [name, value] : overrides) {
            (void)value;
            if (name == key) {
                overridden = true;
                break;
            }
        }
        if (!overridden)
            out.push_back(text);
    }
    for (const auto& [name, value] : overrides) {
        std::string entry = name;
        entry += '=';
        entry += value;
        out.push_back(std::move(entry));
    }
    return out;
}

// Spawn git with stdin inherited and stdout+stderr fused onto one pipe. The
// argv/envp pointer arrays live only for the spawn call.
bool spawn_captured(const std::vector<std::string>& argv_storage,
                    const std::vector<std::string>& env_storage, pid_t& pid, int& read_fd) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return false;
    std::vector<char*> argv;
    for (auto& arg : const_cast<std::vector<std::string>&>(argv_storage))
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& entry : const_cast<std::vector<std::string>&>(env_storage))
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const int spawned = posix_spawnp(&pid, "git", &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        return false;
    }
    read_fd = fds[0];
    return true;
}

std::string drain(int fd) {
    std::string output;
    char buffer[512];
    ssize_t got = 0;
    while ((got = read(fd, buffer, sizeof buffer)) > 0)
        output.append(buffer, static_cast<std::size_t>(got));
    close(fd);
    return output;
}

int wait_exit_status(pid_t pid) {
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

GitResult run_git(const std::string& repo, const std::vector<std::string>& args,
                  const std::vector<std::pair<std::string, std::string>>& env) {
    GitResult result;
    std::vector<std::string> argv_storage{"git", "-C", repo};
    for (const auto& arg : args)
        argv_storage.push_back(arg);
    std::vector<std::string> env_storage = child_environment(env);
    pid_t pid = -1;
    int read_fd = -1;
    if (!spawn_captured(argv_storage, env_storage, pid, read_fd))
        return result;
    result.output = drain(read_fd);
    result.status = wait_exit_status(pid);
    return result;
}

} // namespace agent
