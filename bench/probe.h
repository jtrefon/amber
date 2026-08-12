#ifndef BENCH_PROBE_H
#define BENCH_PROBE_H

// Harness benchmark probes (see docs/spec/benchmark/harness.md): deterministic
// checks that exercise the engine over its public APIs. Each probe feeds a
// fixed input (canned SSE bytes, canned text, scripted tool calls, a context
// op sequence, a workspace layout) and asserts the exact engine output, so a
// deviation can only be a harness bug. Probes never touch a live model.

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bench {

struct ProbeResult {
    std::string family;     // parse | extract | dispatch | context | recovery | ...
    std::string name;
    bool passed = false;
    std::string detail;     // what the engine produced
    std::string expected;   // what was required
    double ms = 0.0;        // execution time
};

struct ProbeFamily {
    std::string name;
    std::string description;
    std::function<bool(ProbeResult&)> run;
};

// Families that must be covered for the harness scorecard to be complete.
inline const std::vector<std::string>& required_probe_families() noexcept {
    static const std::vector<std::string> f = {
        "parse", "extract", "dispatch", "context", "recovery",
        "envelope", "budget", "confinement", "oracle", "loop",
        "fidelity", "output",
    };
    return f;
}

// Run every registered probe. Deterministic: same tree -> same result set.
std::vector<ProbeResult> run_all_probes();

struct HarnessScorecard {
    std::vector<ProbeResult> probes;
    std::map<std::string, std::pair<int, int>> families;  // name -> (passed, total)
    int passed = 0;
    int total = 0;
    double integrity = 0.0;   // passed / total

    double family_integrity(const std::string& family) const noexcept;
};

// Aggregate probe results into a scorecard (pass counts per family + overall).
HarnessScorecard aggregate_probes(const std::vector<ProbeResult>& probes);

// Register a probe (linking-time). Self-registration mirrors the TEST macro.
void register_probe(ProbeResult result, const std::function<bool(ProbeResult&)>& run);

} // namespace bench

#endif // BENCH_PROBE_H