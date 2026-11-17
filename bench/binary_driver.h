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

// Where the amber-cli the driver executes lives: beside amber-bench for an
// install, otherwise the working directory of a source checkout. Empty when
// neither is an executable file.
//
// There is deliberately no environment override. The driver exists to run the
// binary the build produced, and a path taken from the environment would let
// the harness execute an arbitrary program under the benchmark's name.
std::string resolve_cli_path(const std::string& exe_path, const std::string& cwd);

// A trace replaces the scenario's scripted replies with recorded provider
// bytes served by a loopback endpoint; the binary is pointed at it instead of
// the MockServer.
struct BinaryE2eOptions {
    std::string trace_path;
    std::string trace_mode = "strict";
};

// `cli_path` is the amber-cli executable resolved by resolve_cli_path. A
// scenario runs once in-process for the expected tool sequence, then once
// through the binary against MockServer (or the trace endpoint).
std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path, std::string& err);
std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path,
                                                  const BinaryE2eOptions& options,
                                                  std::string& err);

} // namespace bench

#endif // BENCH_BINARY_DRIVER_H
