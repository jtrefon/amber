
#ifndef BENCH_ORACLE_H
#define BENCH_ORACLE_H

// Oracle scoring: match the observed tool-call sequence against the scenario's
// expected steps (ordered, wildcard args, unordered sets) and produce the
// correctness KPIs. Pure functions — no I/O.

#include <string>
#include <vector>

#include "bench/scenario.h"

namespace bench {

struct ToolCallEvent {
    std::string name;
    agent::json args;
    long t_ms = 0;              // wall time of the call (recorder-stamped)
};

struct OracleResult {
    bool success = false;
    double bullseye = 0.0;         // matched steps / total steps
    int matched_steps = 0;
    int total_steps = 0;
    int on_oracle_calls = 0;       // calls that matched a step
    int total_calls = 0;
    double arg_precision = 0.0;    // matched arg keys / expected arg keys
    int wasted = 0;                // off-oracle calls
    int redundant = 0;             // repeated identical (tool, args)
    std::vector<size_t> matched_call_indexes;  // call indexes per matched step
};

// Score a call sequence against the oracle. Empty oracle: success with a full
// bullseye (checks decide the outcome instead).
OracleResult score_oracle(const std::vector<ScenarioStep>& oracle,
                          const std::vector<ToolCallEvent>& calls);

} // namespace bench

#endif // BENCH_ORACLE_H
