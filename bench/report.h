
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
    // Per-call telemetry (BENCH-11): the post-mortem story for every
    // executed call — status, error text, timeout/denied flags, duration.
    struct ToolDetail {
        std::string name;
        std::string args;
        std::string status;     // ok | error | denied | timeout
        std::string error;
        bool denied = false;
        bool timeout = false;
        long duration_ms = 0;
    };
    std::vector<ToolDetail> tool_details;
    // Calls issued within a single step (one LLM turn): max + total.
    int max_calls_per_step = 0;
    int total_steps = 0;
    // Plan metrics (BENCH-09): adherence to the oracle in dependency order,
    // adaptation after a failure, and dependency-order violations.
    double plan_adherence_ratio = 0.0;   // oracle steps matched in order / total
    bool replan_adapted = false;         // a different call followed a failure
    bool dependency_violation = false;   // ordered oracle steps matched out of order
    // Loop-control metrics: how fast a loop broke, and whether steering worked.
    int breakout_latency = 0;            // steps until loop detection fired (0 = none)
    bool steer_effective = false;        // received a steer AND completed
    // calls_per_step distribution: mean + p95 (max is max_calls_per_step).
    double calls_per_step_mean = 0.0;
    double calls_per_step_p95 = 0.0;
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
// a vector of ScenarioReport). Weights are KEYED BY SCENARIO NAME — a
// reordered or incomplete run still aligns correctly.
std::map<std::string, double> discrimination_weights(
    const std::vector<std::vector<ScenarioReport>>& population) noexcept;

// Difficulty x discrimination weighted score. A scenario missing from the
// weight map contributes nothing (weight 0). An empty map falls back to the
// plain difficulty-weighted score (single-file runs have no population).
double run_score_discriminative(
    const std::vector<ScenarioReport>& reports,
    const std::map<std::string, double>& weights) noexcept;

// 95% CI for the median-of-medians model score under the given weights
// (bootstrap of the weighted score). With an empty weight map this is the
// plain difficulty-weighted CI; with discrimination weights it matches
// run_score_discriminative. Returns -1.0 when any scenario lacks repeat data.
double model_score_ci(const std::vector<ScenarioReport>& reports,
                      const std::map<std::string, double>& weights = {}) noexcept;

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

// The resolution rule: two models differ meaningfully only when their score
// gap exceeds the combined confidence interval.
bool resolvable(double ci_a, double score_a, double ci_b,
                double score_b) noexcept;

// BENCH-03 — reference-anchored calibration (the headroom contract).
// The reference model (index into the population) must land at the anchor
// (target = 50 on the 0-100 scale, i.e. 500/1000): the ladder de-weights what
// the reference solves well and weights what it fails, leaving the top of the
// chart open for larger models. Reporting + guidance only — difficulties are
// tuned by the maintainer from the suggestions.

// The reference's plain difficulty-weighted score.
double reference_score(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference) noexcept;

// Continuous per-scenario weights centering the reference exactly at the
// anchor for a balanced population (w = target / |score - target|, clamped).
std::vector<double> anchor_weights(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target = 50.0) noexcept;

// Integer difficulty suggestions in [1, 6], monotonic in the reference score;
// applying them never worsens the anchor deviation.
std::vector<int> suggest_difficulties(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target = 50.0) noexcept;

// |reference_score - target|.
double reference_anchor_deviation(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target = 50.0) noexcept;

// 100 - best model score in the population: the chart must have a top.
double headroom(
    const std::vector<std::vector<ScenarioReport>>& population) noexcept;

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

// Full diagnostic scorecard: dimension-by-dimension KPI aggregates with
// verdicts, per-suite matrix, per-failed-scenario diagnosis, and run-wide
// signals — the "where is the harness failing, and how" view.
std::string render_scorecard(const std::vector<ScenarioReport>& reports,
                             const RunMeta& meta);

} // namespace bench

#endif // BENCH_REPORT_H
