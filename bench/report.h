
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
    Agentic agentic;
    int difficulty = 3;
    std::string reasoning;               // cfg.thinking at run time (live)
    std::string final_text;              // the agent's final answer
    std::vector<std::pair<std::string, std::string>> tool_calls;  // name, args
    bool templated = false;              // static-template scenario
    std::vector<std::string> failures;

    // Repeat aggregation (BENCH-01): when a scenario ran N times, the report
    // is the median run and these carry the population statistics. The raw
    // per-run totals stay in repeat_scores for audit.
    int repeat_n = 1;
    double score_median = 0.0;
    double score_stddev = 0.0;
    std::vector<double> repeat_scores;
};

// Difficulty-weighted aggregate of per-scenario totals: 0..100 (×10 = /1000).
// With repeats, pass the aggregated (median) reports — see aggregate_repeats.
double run_score(const std::vector<ScenarioReport>& reports) noexcept;

// BENCH-02 — discrimination-weighted aggregation (the resolution engine).
// Participation trophies must not move the score: each scenario's weight is
// the standard deviation of its totals across the model population, so a
// scenario everyone solves contributes ~0 and a separator dominates the
// deltas between near-identical models.
// Population = the per-model runs fed to a comparison (vector of runs, each
// a vector of ScenarioReport). One weight per scenario, aligned by name.
std::vector<double> discrimination_weights(
    const std::vector<std::vector<ScenarioReport>>& population) noexcept;

// Difficulty x discrimination weighted score. Empty weights fall back to the
// plain difficulty-weighted score (single-file runs have no population).
double run_score_discriminative(const std::vector<ScenarioReport>& reports,
                                const std::vector<double>& weights) noexcept;

// Median of a sample.
double median(std::vector<double> values) noexcept;

// Sample standard deviation (0 when n < 2).
double stddev(const std::vector<double>& values) noexcept;

// Collapse N runs of one scenario into a single median report carrying the
// population statistics. Single-run input still initializes the metadata
// (repeat_n = 1, score_median = score, repeat_scores = {score}).
ScenarioReport aggregate_repeats(const std::vector<ScenarioReport>& runs);

// 95% confidence interval for a model score built from per-scenario medians
// (difficulty-weighted pooled variance): 1.96 * sqrt(sum(w^2 * s_i^2)) / sum(w).
double model_score_ci(const std::vector<ScenarioReport>& reports) noexcept;

// The resolution rule: two models differ meaningfully only when their score
// gap exceeds the combined confidence interval.
bool resolvable(double ci_a, double score_a, double ci_b,
                double score_b) noexcept;

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

std::string render_json(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

// Rehydrate a stored JSON report (render_json output) into reports + meta.
// Legacy files without the repeat fields default score_median to score.
bool parse_report_json(const agent::json& j, RunMeta& meta,
                       std::vector<ScenarioReport>& reports);

// Markdown report: score table + failure details (for BENCHMARK.md).
std::string render_markdown(const std::vector<ScenarioReport>& reports,
                            const RunMeta& meta);

// Markdown comparison of multiple model runs: scenario × model score matrix
// followed by per-model detail sections.
std::string render_markdown_comparison(
    const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs);

} // namespace bench

#endif // BENCH_REPORT_H
