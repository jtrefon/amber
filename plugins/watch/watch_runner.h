#ifndef AMBER_PLUGINS_WATCH_RUNNER_H
#define AMBER_PLUGINS_WATCH_RUNNER_H

#include "agent/job.h"
#include "agent/process.h"

#include <string>

namespace agent::plugins {

// The watch's command port. Production binds to JobService; tests bind a fake
// that returns canned output/exit codes without spawning anything.
class CommandRunner {
public:
    struct Result {
        std::string output;
        int exit_code = -1;
        bool timed_out = false;
        bool cancelled = false;
    };

    virtual ~CommandRunner() = default;
    virtual Result run(const std::string& command, const std::string& cwd, int timeout_s,
                       const CancellationToken& cancel) = 0;
};

class JobCommandRunner final : public CommandRunner {
public:
    explicit JobCommandRunner(JobService& jobs) : jobs_(jobs) {}

    Result run(const std::string& command, const std::string& cwd, int timeout_s,
               const CancellationToken& cancel) override;

private:
    JobService& jobs_;
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_RUNNER_H
