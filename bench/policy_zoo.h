#ifndef BENCH_POLICY_ZOO_H
#define BENCH_POLICY_ZOO_H

// Named deterministic policies that deliberately behave well or badly. The
// zoo is the benchmark's discrimination test: a scenario should score the
// planner high and the lazy/hallucinating/quitting policies low.

#include <memory>
#include <string>
#include <vector>

#include "bench/policy.h"

namespace bench {

struct Scenario;

struct PolicyMatrixRow {
    std::string scenario;
    std::string policy;
    bool success = false;
    double score = 0.0;
    int recoveries = 0;
    int tool_failures = 0;
    std::vector<std::string> failures;
};

std::vector<std::string> policy_zoo_names();
bool is_policy_zoo_name(const std::string& name);

// `planner` replays the scenario's own state-machine policy. The other names
// are fixed behaviors over the first file in the scenario setup.
std::unique_ptr<Policy> make_policy_zoo_member(const std::string& name, const Scenario& scenario,
                                               std::string& err);

// Compare matrix rows with `bench/policies/expectations.json`. Returns false
// with the first mismatch in `err`.
bool check_policy_expectations(const std::string& path, const std::vector<PolicyMatrixRow>& rows,
                               std::string& err);

} // namespace bench

#endif // BENCH_POLICY_ZOO_H
