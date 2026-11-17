#ifndef BENCH_WORKSPACE_CHECK_H
#define BENCH_WORKSPACE_CHECK_H

#include <filesystem>
#include <string>
#include <vector>

#include "bench/scenario.h"

namespace bench {

// Compare the run's workspace with the scenario's `expected_workspace` block.
// Returns one human-readable failure per missing, extra or mismatched item.
std::vector<std::string> check_expected_workspace(const Scenario& scenario,
                                                  const std::filesystem::path& workspace);

} // namespace bench

#endif // BENCH_WORKSPACE_CHECK_H
