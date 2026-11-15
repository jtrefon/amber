#ifndef BENCH_BINARY_DRIVER_H
#define BENCH_BINARY_DRIVER_H

// End-to-end driver: run a scenario through the real amber-cli binary against
// a scripted HTTP endpoint, then compare its telemetry with the in-process
// runner. This is the only tier that boots main(), config loading, plugins and
// the tool registry as a user does.

#include <string>
#include <vector>

#include "bench/scenario.h"

namespace bench {

struct BinaryE2eResult {
    std::string name;
    bool ok = false;
    std::string error;
    int exit_code = -1;
    std::vector<std::string> expected_tools;
    std::vector<std::string> actual_tools;
};

// `cli_path` is the amber-cli executable; callers pass AMBER_BENCH_CLI or
// "./amber-cli". A scenario runs once in-process for the expected tool
// sequence, then once through the binary against MockServer.
std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path, std::string& err);

} // namespace bench

#endif // BENCH_BINARY_DRIVER_H
