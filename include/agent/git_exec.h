#ifndef AGENT_GIT_EXEC_H
#define AGENT_GIT_EXEC_H

#include <string>
#include <utility>
#include <vector>

namespace agent {

struct GitResult {
    int status = -1;    // -1 when git could not be started
    std::string output; // merged stdout + stderr
};

// Run `git -C <repo> <args...>` with argv exec — never a shell string, because
// model-supplied names can reach it — and capture merged stdout+stderr: git's
// own refusals are the messages the user ends up reading.
//
// `env` entries are set in the child and override the parent's. The child
// environment is built before spawning, so a multithreaded caller never has to
// mutate the process environment.
GitResult run_git(const std::string& repo, const std::vector<std::string>& args,
                  const std::vector<std::pair<std::string, std::string>>& env = {});

} // namespace agent

#endif // AGENT_GIT_EXEC_H
