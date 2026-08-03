
#ifndef BENCH_REPORT_H
#define BENCH_REPORT_H

// Report rendering: per-scenario KPI records aggregated into text/JSON output.

#include <string>
#include <vector>

#include "bench/kpi.h"
#include "bench/scenario.h"

namespace bench {

struct RunMeta {
    std::string run_id;
    std::string mode;         // "hermetic" | "live"
    std::string profile;
    std::string model;
    std::string reasoning;    // "on" | "off" | "auto" (cfg.thinking)
    std::string engine_version;
    std::string timestamp;
};

struct ScenarioReport {
    std::string name;
    std::string suite;
    Kpi kpi;
    Score score;
    int difficulty = 3;
    std::string reasoning;               // cfg.thinking at run time (live)
    std::string final_text;              // the agent's final answer
    std::vector<std::pair<std::string, std::string>> tool_calls;  // name, args
    bool templated = false;              // static-template scenario
    std::vector<std::string> failures;
};

// Difficulty-weighted aggregate of per-scenario totals: 0..100 (×10 = /1000).
double run_score(const std::vector<ScenarioReport>& reports) noexcept;

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

std::string render_json(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

// Markdown report: score table + failure details (for BENCHMARK.md).
std::string render_markdown(const std::vector<ScenarioReport>& reports,
                            const RunMeta& meta);

// Markdown comparison of multiple model runs: scenario × model score matrix
// followed by per-model detail sections.
std::string render_markdown_comparison(
    const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs);

} // namespace bench

#endif // BENCH_REPORT_H
