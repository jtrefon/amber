#include "agent/git_exec.h"

#include <cerrno>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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
    for (const auto& [name, value] : overrides)
        out.push_back(name + "=" + value);
    return out;
}

} // namespace

GitResult run_git(const std::string& repo, const std::vector<std::string>& args,
                  const std::vector<std::pair<std::string, std::string>>& env) {
    GitResult result;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return result;

    std::vector<std::string> argv_storage{"git", "-C", repo};
    for (const auto& arg : args)
        argv_storage.push_back(arg);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = child_environment(env);
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& entry : env_storage)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, "git", &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        return result;
    }

    char buffer[512];
    ssize_t got = 0;
    while ((got = read(fds[0], buffer, sizeof buffer)) > 0)
        result.output.append(buffer, static_cast<std::size_t>(got));
    close(fds[0]);

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited > 0 && WIFEXITED(status))
        result.status = WEXITSTATUS(status);
    return result;
}

} // namespace agent
