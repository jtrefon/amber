
#include "bench/report.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

#include "agent/tool.h"

namespace bench {

double run_score(const std::vector<ScenarioReport>& reports) noexcept {
    double weighted = 0.0;
    int weight = 0;
    for (const auto& r : reports) {
        weighted += r.difficulty * r.score.total;
        weight += r.difficulty;
    }
    return weight > 0 ? weighted / weight : 100.0;
}

namespace {
// Per scenario name: the totals across the models that ran it.
std::map<std::string, std::vector<double>> scenario_totals(
    const std::vector<std::vector<ScenarioReport>>& population) {
    std::map<std::string, std::vector<double>> totals;
    for (const auto& run : population)
        for (const auto& r : run) totals[r.name].push_back(r.score.total);
    return totals;
}
} // namespace

std::map<std::string, double> discrimination_weights(
    const std::vector<std::vector<ScenarioReport>>& population) noexcept {
    std::map<std::string, double> weights;
    const auto totals = scenario_totals(population);
    for (const auto& [name, scores] : totals)
        weights[name] = stddev(scores);
    return weights;
}

double run_score_discriminative(
    const std::vector<ScenarioReport>& reports,
    const std::map<std::string, double>& weights) noexcept {
    if (weights.empty()) return run_score(reports);
    double weighted = 0.0;
    double weight_sum = 0.0;
    for (const auto& r : reports) {
        const auto it = weights.find(r.name);
        if (it == weights.end()) continue;  // unknown scenario: no signal
        const double w = it->second * static_cast<double>(r.difficulty);
        weighted += w * r.score.total;
        weight_sum += w;
    }
    return weight_sum > 0.0 ? weighted / weight_sum : run_score(reports);
}

double median(std::vector<double> values) noexcept {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1
               ? values[n / 2]
               : (values[(n / 2) - 1] + values[n / 2]) / 2.0;
}

double stddev(const std::vector<double>& values) noexcept {
    if (values.size() < 2) return 0.0;
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double sq = 0.0;
    for (const double v : values) sq += (v - mean) * (v - mean);
    return std::sqrt(sq / (values.size() - 1));
}

namespace {
// Run whose total sits closest to the median — the representative report.
size_t median_representative(const std::vector<ScenarioReport>& runs,
                             double score_median) {
    size_t best = 0;
    auto closest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < runs.size(); ++i) {
        const double d = std::abs(runs[i].score.total - score_median);
        if (d < closest) { closest = d; best = i; }
    }
    return best;
}

// The representative run's detail fields (kpi/agentic/tool_calls/final_text).
void copy_representative_details(ScenarioReport& agg, const ScenarioReport& rep) {
    agg.kpi = rep.kpi;
    agg.score = rep.score;
    agg.agentic = rep.agentic;
    agg.final_text = rep.final_text;
    agg.tool_calls = rep.tool_calls;
    agg.failures = rep.failures;
    agg.templated = rep.templated;
}
} // namespace

ScenarioReport aggregate_repeats(const std::vector<ScenarioReport>& runs) {
    if (runs.empty()) return {};
    ScenarioReport agg = runs.front();
    agg.repeat_n = static_cast<int>(runs.size());
    agg.repeat_scores.clear();
    for (const auto& r : runs) agg.repeat_scores.push_back(r.score.total);
    if (runs.size() == 1) {
        // Single runs still carry the metadata: median = the score itself.
        agg.score_median = agg.score.total;
        return agg;
    }
    agg.score_median = median(agg.repeat_scores);
    agg.score_stddev = stddev(agg.repeat_scores);
    copy_representative_details(agg,
                                runs[median_representative(runs, agg.score_median)]);
    // The canonical score IS the median; the representative run only carries
    // the detail fields (kpi/agentic/tool_calls).
    agg.score.total = agg.score_median;
    return agg;
}

namespace {
// Deterministic LCG so the bootstrap is reproducible across runs and tests.
uint32_t lcg(uint32_t& state) noexcept {
    state = (state * 1664525u) + 1013904223u;
    return state;
}

// One bootstrap iteration: resample each scenario's repeat_scores, take the
// per-scenario median, and return the (discrimination x difficulty) weighted
// model score. An empty weight map reproduces the plain difficulty weighting.
double bootstrap_weighted_score(
    const std::vector<ScenarioReport>& reports, uint32_t& rng,
    const std::map<std::string, double>& weights) noexcept {
    double weighted = 0.0;
    double weight = 0.0;
    for (const auto& r : reports) {
        const auto& pool = r.repeat_scores;
        std::vector<double> sample;
        sample.reserve(pool.size());
        for (size_t i = 0; i < pool.size(); ++i)
            sample.push_back(pool[lcg(rng) % pool.size()]);
        auto w = static_cast<double>(r.difficulty);
        const auto it = weights.find(r.name);
        if (it != weights.end()) w *= it->second;
        weighted += w * median(std::move(sample));
        weight += w;
    }
    return weight > 0.0 ? weighted / weight : 100.0;
}
} // namespace

// 95% CI for the median-of-medians model score, estimated by bootstrap
// resampling of each scenario's repeat_scores (uncertainty shrinks as
// repeat_n grows). Uses the same weighting as the score it describes — pass
// the discrimination weights to match run_score_discriminative. Returns -1.0
// when any scenario lacks repeat data — the CI is missing, never silently
// zero (a single run must not claim precision).
double model_score_ci(const std::vector<ScenarioReport>& reports,
                      const std::map<std::string, double>& weights) noexcept {
    for (const auto& r : reports)
        if (r.repeat_n < 2 || r.repeat_scores.size() < 2) return -1.0;
    uint32_t rng = 0xC0FFEEu;
    std::vector<double> boot;
    boot.reserve(200);
    for (int i = 0; i < 200; ++i)
        boot.push_back(bootstrap_weighted_score(reports, rng, weights));
    return 1.96 * stddev(boot);
}

bool resolvable(double ci_a, double score_a, double ci_b,
                double score_b) noexcept {
    const double gap = std::abs(score_a - score_b);
    // ci_a/ci_b are already 95% intervals; the combined interval of the
    // difference is their root-sum-square (independent runs).
    const double combined = std::sqrt((ci_a * ci_a) + (ci_b * ci_b));
    return gap > combined;
}

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta) {
    std::ostringstream out;
    out << "amber-bench run " << meta.run_id << " [" << meta.mode << ", "
        << (meta.profile.empty() ? "default" : meta.profile) << ", "
        << meta.model << "]\n";
    out << "engine " << meta.engine_version << " @ " << meta.timestamp << "\n";
    const double ci = model_score_ci(reports);  // difficulty-weighted for single runs
    out << "model score: " << (run_score(reports) * 10.0) << "/1000";
    if (ci > 0.0) {
        std::ostringstream ci_s;
        ci_s << std::fixed << std::setprecision(1) << (ci * 10.0);
        out << "  ±" << ci_s.str() << " (95% CI)";
    } else {
        out << "  (single-run: no CI)";
    }
    out << "\n";
    int passed = 0;
    for (const auto& r : reports) {
        const char* verdict = r.kpi.success ? "PASS" : "FAIL";
        if (r.kpi.success) ++passed;
        out << verdict << "  " << r.name << " (" << r.suite << ")"
            << "  score=" << static_cast<int>(r.score.total)
            << (r.repeat_n > 1
                    ? [&r]() {
                          std::ostringstream ss;
                          ss << std::fixed << std::setprecision(1)
                             << " (median " << r.score_median << ", σ "
                             << r.score_stddev << ")";
                          return ss.str();
                      }()
                    : "")
            << " d" << r.difficulty
            << " bullseye=" << r.kpi.bullseye
            << " steps=" << r.kpi.steps
            << " cd=" << r.kpi.bash_cd_prefix
            << " wasted=" << r.kpi.wasted
            << " wall=" << r.kpi.wall_ms << "ms"
            << " retries=" << r.kpi.retries
            << " recoveries=" << r.kpi.recoveries;
        if (r.templated) {
            out << " artifact=" << r.kpi.artifact_score
                << " compile=" << (r.kpi.compile_ok ? "ok" : "FAIL")
                << " behavior=" << (r.kpi.behavior_equivalent ? "eq" : "diff");
        }
        out << "\n";
        if (!r.failures.empty()) {
            for (const auto& f : r.failures) out << "    - " << f << "\n";
            const size_t cap = std::min<size_t>(r.tool_calls.size(), 12);
            for (size_t i = 0; i < cap; ++i) {
                const auto& tc = r.tool_calls[i];
                std::string a = tc.second;
                if (a.size() > 80) {
                    a.resize(77);
                    a += "...";
                }
                out << "    call: " << tc.first << " " << a << "\n";
            }
            if (r.tool_calls.size() > cap)
                out << "    ... " << (r.tool_calls.size() - cap)
                    << " more calls (full trace in JSON)\n";
            if (!r.final_text.empty()) {
                std::string t = r.final_text;
                if (t.size() > 140) {
                    t.resize(137);
                    t += "...";
                }
                out << "    final: " << t << "\n";
            }
        }
    }
    out << passed << "/" << reports.size() << " scenarios passed\n";
    return out.str();
}

std::string render_json(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta) {
    agent::json out = agent::json::object();
    out["run_id"] = meta.run_id;
    out["mode"] = meta.mode;
    out["profile"] = meta.profile;
    out["model"] = meta.model;
    out["engine_version"] = meta.engine_version;
    out["timestamp"] = meta.timestamp;
    out["reasoning"] = meta.reasoning;
    agent::json arr = agent::json::array();
    for (const auto& r : reports) {
        agent::json j = agent::json::object();
        j["name"] = r.name;
        j["suite"] = r.suite;
        j["success"] = r.kpi.success;
        j["difficulty"] = r.difficulty;
        j["score"] = r.score.total;
        j["repeat_n"] = r.repeat_n;
        j["score_median"] = r.score_median;
        j["score_stddev"] = r.score_stddev;
        if (!r.repeat_scores.empty()) j["repeat_scores"] = r.repeat_scores;
        j["score_correctness"] = r.score.correctness;
        j["score_efficiency"] = r.score.efficiency;
        j["score_robustness"] = r.score.robustness;
        j["score_adherence"] = r.score.adherence;
        j["bullseye"] = r.kpi.bullseye;
        j["tool_call_accuracy"] = r.kpi.tool_call_accuracy;
        j["arg_precision"] = r.kpi.arg_precision;
        j["steps"] = r.kpi.steps;
        j["compressions"] = r.kpi.compressions;
        j["bash_cd_prefix"] = r.kpi.bash_cd_prefix;
        j["tool_calls_total"] = r.kpi.tool_calls;
        j["tool_failures"] = r.kpi.tool_failures;
        j["tool_denied"] = r.kpi.tool_denied;
        j["wasted"] = r.kpi.wasted;
        j["redundant"] = r.kpi.redundant;
        j["retries"] = r.kpi.retries;
        j["recoveries"] = r.kpi.recoveries;
        j["hard_stop"] = r.kpi.hard_stop;
        j["wall_ms"] = r.kpi.wall_ms;
        j["bullseye_at_ms"] = r.kpi.bullseye_at_ms;
        j["ttft_ms"] = r.kpi.ttft_ms;
        j["tps_avg"] = r.kpi.tps_avg;
        j["prompt_tokens"] = r.kpi.prompt_tokens;
        j["completion_tokens"] = r.kpi.completion_tokens;
        j["baseline_rss_kb"] = r.kpi.baseline_rss_kb;
        j["peak_rss_kb"] = r.kpi.peak_rss_kb;
        j["cpu_ms"] = r.kpi.cpu_ms;
        j["files_touched"] = r.kpi.files_touched;
        j["artifact_score"] = r.kpi.artifact_score;
        j["compile_ok"] = r.kpi.compile_ok;
        j["behavior_equivalent"] = r.kpi.behavior_equivalent;
        j["structure_checks"] = r.kpi.structure_checks;
        j["prompt_adherence"] = r.kpi.prompt_adherence;
        j["agentic_has_plan"] = r.agentic.has_plan;
        j["agentic_plan_tools"] = r.agentic.plan_tools;
        j["agentic_deviation"] = r.agentic.plan_deviation;
        j["agentic_ratio"] = r.agentic.plan_ratio;
        j["agentic_efficiency_pct"] = r.agentic.efficiency_pct;
        j["agentic_score"] = r.agentic.score;
        j["agentic_plan_by_tool"] = r.agentic.plan_by_tool;
        j["agentic_actual_by_tool"] = r.agentic.actual_by_tool;
        j["templated"] = r.templated;
        j["final_text"] = r.final_text;
        agent::json calls = agent::json::array();
        for (const auto& tc : r.tool_calls) {
            agent::json cj;
            cj["tool"] = tc.first;
            cj["args"] = tc.second;
            calls.push_back(std::move(cj));
        }
        j["tool_calls"] = std::move(calls);
        // Per-call telemetry (BENCH-11): the post-mortem story.
        agent::json details = agent::json::array();
        for (const auto& d : r.tool_details) {
            agent::json dj;
            dj["tool"] = d.name;
            dj["args"] = d.args;
            dj["status"] = d.status;
            dj["error"] = d.error;
            dj["denied"] = d.denied;
            dj["timeout"] = d.timeout;
            dj["duration_ms"] = d.duration_ms;
            details.push_back(std::move(dj));
        }
        j["tool_details"] = std::move(details);
        j["max_calls_per_step"] = r.max_calls_per_step;
        j["total_steps"] = r.total_steps;
        j["plan_adherence_ratio"] = r.plan_adherence_ratio;
        j["replan_adapted"] = r.replan_adapted;
        j["dependency_violation"] = r.dependency_violation;
        j["breakout_latency"] = r.breakout_latency;
        j["steer_effective"] = r.steer_effective;
        j["calls_per_step_mean"] = r.calls_per_step_mean;
        j["calls_per_step_p95"] = r.calls_per_step_p95;
        agent::json taxonomy = agent::json::object();
        for (const auto& tf : r.kpi.failure_taxonomy)
            taxonomy[tf.first] = tf.second;
        j["failure_taxonomy"] = std::move(taxonomy);
        j["failures"] = r.failures;
        arr.push_back(std::move(j));
    }
    out["scenarios"] = std::move(arr);
    out["model_score"] = run_score(reports) * 10.0;
    out["model_score_ci"] = model_score_ci(reports) * 10.0;
    return out.dump(2) + "\n";
}

std::string render_markdown(const std::vector<ScenarioReport>& reports,
                            const RunMeta& meta) {
    std::ostringstream out;
    out << "## " << meta.model << "\n\n";
    out << "- run: `" << meta.run_id << "` [" << meta.mode << ", engine "
        << meta.engine_version << ", reasoning "
        << (meta.reasoning.empty() ? "default" : meta.reasoning) << "]\n";
    out << "- **model score: " << static_cast<int>(run_score(reports) * 10.0)
        << "/1000** (" << reports.size() << " scenarios)\n\n";
    out << "| scenario | d | score | bullseye | steps | wasted | wall (s) "
           "| artifact |\n";
    out << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& r : reports) {
        out << "| " << r.name << " | " << r.difficulty << " | "
            << static_cast<int>(r.score.total) << " | " << r.kpi.bullseye
            << " | " << r.kpi.steps << " | " << r.kpi.wasted << " | "
            << (r.kpi.wall_ms / 1000.0) << " | ";
        if (r.templated)
            out << r.kpi.artifact_score;
        else
            out << "-";
        out << " |\n";
    }
    out << "\n### Failures\n\n";
    bool any = false;
    for (const auto& r : reports) {
        if (r.kpi.success) continue;
        any = true;
        out << "- **" << r.name << "** (" << static_cast<int>(r.score.total)
            << "/100): ";
        for (size_t i = 0; i < r.failures.size(); ++i) {
            if (i) out << "; ";
            out << r.failures[i];
        }
        out << "\n";
    }
    if (!any) out << "none\n";

    out << "\n### Agentic profile\n\n";
    int tot_tools = 0, tot_fail = 0, tot_denied = 0, tot_red = 0, tot_retr = 0;
    long tot_wall = 0;
    int plan_total = 0, dev_total = 0, ag_n = 0;
    double ag_sum = 0.0;
    for (const auto& r : reports) {
        tot_tools += r.kpi.tool_calls;
        tot_fail += r.kpi.tool_failures;
        tot_denied += r.kpi.tool_denied;
        tot_red += r.kpi.redundant;
        tot_retr += r.kpi.retries;
        tot_wall += r.kpi.wall_ms;
        if (r.agentic.has_plan) {
            plan_total += r.agentic.plan_tools;
            dev_total += r.agentic.plan_deviation;
            ag_sum += r.agentic.score;
            ++ag_n;
        }
    }
    const int n = static_cast<int>(reports.size());
    out << "| metric | total | per scenario |\n|---|---|---|\n";
    out << "| tool calls | " << tot_tools << " | "
        << (n ? (tot_tools / static_cast<double>(n)) : 0.0) << " |\n";
    int tot_cd = 0;
    for (const auto& r : reports) tot_cd += r.kpi.bash_cd_prefix;
    out << "| bash cd-prefix calls | " << tot_cd << " | "
        << (n ? (tot_cd / static_cast<double>(n)) : 0.0) << " |\n";
    out << "| tool failures | " << tot_fail << " | "
        << (n ? (tot_fail / static_cast<double>(n)) : 0.0) << " |\n";
    out << "| tool denials | " << tot_denied << " | "
        << (n ? (tot_denied / static_cast<double>(n)) : 0.0) << " |\n";
    out << "| redundant calls | " << tot_red << " | "
        << (n ? (tot_red / static_cast<double>(n)) : 0.0) << " |\n";
    out << "| LLM retries | " << tot_retr << " | "
        << (n ? (tot_retr / static_cast<double>(n)) : 0.0) << " |\n";
    out << "| wall time (s) | " << (tot_wall / 1000.0) << " | "
        << (n ? (tot_wall / 1000.0 / n) : 0.0) << " |\n";
    if (ag_n > 0) {
        out << "\n**Plan adherence** (optimal tool plan vs actual):\n\n";
        out << "| metric | value |\n|---|---|\n";
        out << "| scenarios with a plan | " << ag_n << " |\n";
        out << "| optimal tool calls (sum) | " << plan_total << " |\n";
        out << "| actual tool calls | " << tot_tools << " |\n";
        out << "| total deviation (extra calls) | " << dev_total << " |\n";
        const double eff = tot_tools > 0
                               ? (100.0 * plan_total / tot_tools)
                               : 0.0;
        out << "| plan efficiency | " << static_cast<int>(eff > 100.0 ? 100.0 : eff)
            << "/100 |\n";
        out << "| agentic score (mean plan adherence) | "
            << static_cast<int>(ag_sum / ag_n) << "/100 |\n";

        std::map<std::string, int> plan_mix, actual_mix;
        for (const auto& r : reports) {
            for (const auto& p : r.agentic.plan_by_tool)
                plan_mix[p.first] += p.second;
            for (const auto& a : r.agentic.actual_by_tool)
                actual_mix[a.first] += a.second;
        }
        out << "\n**Tool mix (plan vs actual, summed across scenarios):**\n\n";
        out << "| tool | plan | actual | deviation | efficiency % |\n"
               "|---|---|---|---|---|\n";
        for (const auto& p : plan_mix) {
            const int act = actual_mix[p.first];
            const double eff = act > 0 ? (100.0 * p.second / act) : 0.0;
            out << "| " << p.first << " | " << p.second << " | " << act
                << " | " << (act - p.second) << " | "
                << static_cast<int>(eff > 100.0 ? 100.0 : eff) << " |\n";
        }
        for (const auto& a : actual_mix) {
            if (plan_mix.count(a.first)) continue;
            out << "| " << a.first << " | 0 | " << a.second << " | "
                << a.second << " | 0 |\n";
        }
    }
    return out.str();
}

std::string render_markdown_comparison(
    const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs) {
    if (runs.empty()) return "";
    std::ostringstream out;
    out << "# Benchmark: harness score by model\n\n";
    // With a population of models, score by discrimination: participation
    // trophies weigh ~0 and separators dominate the deltas (BENCH-02).
    std::vector<std::vector<ScenarioReport>> population;
    population.reserve(runs.size());
    for (const auto& run : runs) population.push_back(run.second);
    const std::map<std::string, double> weights =
        discrimination_weights(population);
    auto score_of = [&](const std::vector<ScenarioReport>& rep) {
        return run_score_discriminative(rep, weights);
    };
    auto ci_of = [&](const std::vector<ScenarioReport>& rep) {
        return model_score_ci(rep, weights);
    };
    for (const auto& run : runs) {
        const double ci = ci_of(run.second);
        out << "- **" << run.first.model << "**: "
            << static_cast<int>(score_of(run.second) * 10.0) << "/1000";
        if (ci > 0.0) {
            std::ostringstream ci_s;
            ci_s << std::fixed << std::setprecision(1) << (ci * 10.0);
            out << " ±" << ci_s.str();
        }
        out << "\n";
    }
    // The resolution rule: differences within the combined CI are noise.
    if (runs.size() >= 2) {
        out << "\n**Resolution** (a gap is a finding only when it exceeds the "
               "combined 95% CI):\n\n";
        for (size_t i = 0; i < runs.size(); ++i)
            for (size_t j = i + 1; j < runs.size(); ++j) {
                const double sa = score_of(runs[i].second);
                const double sb = score_of(runs[j].second);
                const double ca = ci_of(runs[i].second);
                const double cb = ci_of(runs[j].second);
                std::string verdict;
                if (ca < 0.0 || cb < 0.0)
                    verdict = "insufficient repeat data (run --repeat)";
                else
                    verdict = resolvable(ca, sa, cb, sb)
                                  ? "**resolvable**"
                                  : "within noise";
                out << "- " << runs[i].first.model << " vs "
                    << runs[j].first.model << ": Δ="
                    << static_cast<int>(std::abs(sa - sb) * 10.0)
                    << " → " << verdict << "\n";
            }
    }

    out << "\n| model | score | agentic | plan % | tools | fail % | redun % | "
           "steps | wall (s) |\n";
    out << "|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& run : runs) {
        int tools = 0, fail = 0, denied = 0, red = 0, retr = 0, steps = 0;
        long wall = 0;
        double ag_sum = 0.0;
        int ag_n = 0;
        for (const auto& r : run.second) {
            tools += r.kpi.tool_calls;
            fail += r.kpi.tool_failures;
            denied += r.kpi.tool_denied;
            red += r.kpi.redundant;
            retr += r.kpi.retries;
            steps += r.kpi.steps;
            wall += r.kpi.wall_ms;
            if (r.agentic.has_plan) {
                ag_sum += r.agentic.score;
                ++ag_n;
            }
        }
        double plan_pct = 0.0;
        if (ag_n > 0) {
            int pt = 0, at = 0;
            for (const auto& r : run.second) {
                if (!r.agentic.has_plan) continue;
                pt += r.agentic.plan_tools;
                at += r.kpi.tool_calls;
            }
            plan_pct = at > 0 ? (100.0 * pt / at) : 0.0;
            if (plan_pct > 100.0) plan_pct = 100.0;
        }
        const int fail_pct = tools > 0 ? (100 * fail / tools) : 0;
        const int red_pct = tools > 0 ? (100 * red / tools) : 0;
        out << "| " << run.first.model << " | "
            << static_cast<int>(score_of(run.second) * 10.0) << " | "
            << (ag_n ? static_cast<int>(ag_sum / ag_n) : 0) << " | "
            << static_cast<int>(plan_pct) << " | " << tools
            << " | " << fail_pct << " | " << red_pct << " | "
            << steps << " | " << (wall / 1000.0) << " |\n";
    }

    out << "\n| scenario |";
    for (const auto& run : runs) out << " " << run.first.model << " |";
    out << "\n|---|";
    for (size_t i = 0; i < runs.size(); ++i) out << "---|";
    out << "\n";
    const size_t n = runs[0].second.size();
    for (size_t i = 0; i < n; ++i) {
        out << "| " << runs[0].second[i].name << " |";
        for (const auto& run : runs) {
            if (i < run.second.size())
                out << " " << static_cast<int>(run.second[i].score.total)
                    << " |";
            else
                out << " - |";
        }
        out << "\n";
    }

    out << "\n---\n\n";
    for (const auto& run : runs)
        out << render_markdown(run.second, run.first) << "\n";
    return out.str();
}

bool parse_report_json(const agent::json& j, RunMeta& meta,
                       std::vector<ScenarioReport>& reports) {
    if (!j.contains("scenarios") || !j["scenarios"].is_array()) return false;
    meta.run_id = j.value("run_id", "");
    meta.mode = j.value("mode", "");
    meta.profile = j.value("profile", "");
    meta.model = j.value("model", "");
    meta.engine_version = j.value("engine_version", "");
    meta.timestamp = j.value("timestamp", "");
    meta.reasoning = j.value("reasoning", "");
    for (const auto& e : j["scenarios"]) {
        ScenarioReport rep;
        rep.name = e.value("name", "");
        rep.suite = e.value("suite", "");
        rep.kpi.success = e.value("success", false);
        rep.kpi.bullseye = e.value("bullseye", 0.0);
        rep.kpi.steps = e.value("steps", 0);
        rep.kpi.tool_calls = e.value("tool_calls_total", 0);
        rep.kpi.tool_failures = e.value("tool_failures", 0);
        rep.kpi.tool_denied = e.value("tool_denied", 0);
        rep.kpi.redundant = e.value("redundant", 0);
        rep.kpi.retries = e.value("retries", 0);
        rep.kpi.wasted = e.value("wasted", 0);
        rep.kpi.recoveries = e.value("recoveries", 0);
        rep.kpi.wall_ms = e.value("wall_ms", 0L);
        rep.max_calls_per_step = e.value("max_calls_per_step", 0);
        rep.total_steps = e.value("total_steps", 0);
        rep.plan_adherence_ratio = e.value("plan_adherence_ratio", 0.0);
        rep.replan_adapted = e.value("replan_adapted", false);
        rep.dependency_violation = e.value("dependency_violation", false);
        rep.breakout_latency = e.value("breakout_latency", 0);
        rep.steer_effective = e.value("steer_effective", false);
        rep.calls_per_step_mean = e.value("calls_per_step_mean", 0.0);
        rep.calls_per_step_p95 = e.value("calls_per_step_p95", 0.0);
        rep.difficulty = e.value("difficulty", 3);
        rep.score.total = e.value("score", 0.0);
        rep.repeat_n = e.value("repeat_n", 1);
        rep.score_median = e.value("score_median", rep.score.total);
        rep.score_stddev = e.value("score_stddev", 0.0);
        if (e.contains("repeat_scores") && e["repeat_scores"].is_array())
            for (const auto& v : e["repeat_scores"])
                if (v.is_number()) rep.repeat_scores.push_back(v.get<double>());
        rep.templated = e.value("templated", false);
        rep.agentic.has_plan = e.value("agentic_has_plan", false);
        rep.agentic.plan_tools = e.value("agentic_plan_tools", 0);
        rep.agentic.plan_deviation = e.value("agentic_deviation", 0);
        rep.agentic.plan_ratio = e.value("agentic_ratio", 0.0);
        rep.agentic.efficiency_pct = e.value("agentic_efficiency_pct", 0.0);
        rep.agentic.score = e.value("agentic_score", 0.0);
        if (e.contains("agentic_plan_by_tool") &&
            e["agentic_plan_by_tool"].is_object())
            for (auto it = e["agentic_plan_by_tool"].begin();
                 it != e["agentic_plan_by_tool"].end(); ++it)
                if (it.value().is_number_integer())
                    rep.agentic.plan_by_tool[it.key()] =
                        it.value().get<int>();
        if (e.contains("agentic_actual_by_tool") &&
            e["agentic_actual_by_tool"].is_object())
            for (auto it = e["agentic_actual_by_tool"].begin();
                 it != e["agentic_actual_by_tool"].end(); ++it)
                if (it.value().is_number_integer())
                    rep.agentic.actual_by_tool[it.key()] =
                        it.value().get<int>();
        // Failures + per-call telemetry must survive the round trip, or a
        // stored report cannot be diagnosed (the scorecard's section 3).
        if (e.contains("failures") && e["failures"].is_array())
            for (const auto& f : e["failures"])
                if (f.is_string()) rep.failures.push_back(f.get<std::string>());
        if (e.contains("tool_details") && e["tool_details"].is_array()) {
            for (const auto& d : e["tool_details"]) {
                ScenarioReport::ToolDetail td;
                td.name = d.value("tool", "");
                td.args = d.value("args", "");
                td.status = d.value("status", "");
                td.error = d.value("error", "");
                td.denied = d.value("denied", false);
                td.timeout = d.value("timeout", false);
                td.duration_ms = d.value("duration_ms", 0L);
                rep.tool_details.push_back(std::move(td));
            }
        }
        reports.push_back(std::move(rep));
    }
    return true;
}

double reference_score(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference) noexcept {
    if (reference >= population.size()) return 0.0;
    return run_score(population[reference]);
}

std::vector<double> anchor_weights(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target) noexcept {
    std::vector<double> w;
    if (reference >= population.size()) return w;
    for (const auto& r : population[reference]) {
        const double s = r.score.total;
        if (std::abs(s - target) < 0.001) {
            w.push_back(1.0);
            continue;
        }
        double a = target / std::abs(s - target);
        if (a < 0.25) a = 0.25;
        if (a > 200.0) a = 200.0;
        w.push_back(a);
    }
    return w;
}

std::vector<int> suggest_difficulties(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target) noexcept {
    const std::vector<double> w = anchor_weights(population, reference, target);
    std::vector<int> out;
    out.reserve(w.size());
    for (const double a : w) {
        // Map the continuous weight onto the 1..6 ladder, monotonic in the
        // reference score (solved well -> low weight -> low difficulty).
        int d = static_cast<int>(std::lround(6.0 * a / (a + 1.0)));
        if (d < 1) d = 1;
        if (d > 6) d = 6;
        out.push_back(d);
    }
    return out;
}

double reference_anchor_deviation(
    const std::vector<std::vector<ScenarioReport>>& population,
    size_t reference, double target) noexcept {
    return std::abs(reference_score(population, reference) - target);
}

double headroom(
    const std::vector<std::vector<ScenarioReport>>& population) noexcept {
    double best = 0.0;
    for (const auto& run : population) {
        const double s = run_score(run);
        if (s > best) best = s;
    }
    return 100.0 - best;
}

namespace {

struct RunAgg {
    int n = 0;
    double score_sum = 0.0;
    int pass = 0;
    long wasted = 0;
    long redundant = 0;
    long failures = 0;
    long denied = 0;
    long retries = 0;
    long recoveries = 0;
    long steers = 0;
    int hard_stops = 0;
    int replans = 0;
    int dep_violations = 0;
    int breakouts = 0;
    int steers_effective = 0;
    long calls = 0;
    long steps = 0;
    double tps_sum = 0.0;
    int tps_n = 0;
    double bullseye_sum = 0.0;
    double adherence_sum = 0.0;
    long wall_sum = 0;
};

void agg_add(RunAgg& a, const ScenarioReport& r) {
    ++a.n;
    a.score_sum += r.score.total;
    if (r.failures.empty()) ++a.pass;
    a.wasted += r.kpi.wasted;
    a.redundant += r.kpi.redundant;
    a.failures += r.kpi.tool_failures;
    a.denied += r.kpi.tool_denied;
    a.retries += r.kpi.retries;
    a.recoveries += r.kpi.recoveries;
    a.steers += r.kpi.steers;
    if (r.kpi.hard_stop) ++a.hard_stops;
    if (r.replan_adapted) ++a.replans;
    if (r.dependency_violation) ++a.dep_violations;
    if (r.breakout_latency > 0) ++a.breakouts;
    if (r.steer_effective) ++a.steers_effective;
    a.calls += r.kpi.tool_calls;
    a.steps += r.kpi.steps;
    a.bullseye_sum += r.kpi.bullseye;
    a.adherence_sum += r.plan_adherence_ratio;
    a.wall_sum += r.kpi.wall_ms;
    if (r.kpi.tps_avg > 0) {
        a.tps_sum += r.kpi.tps_avg;
        ++a.tps_n;
    }
}

} // namespace

std::string render_scorecard(const std::vector<ScenarioReport>& reports,
                             const RunMeta& meta) {
    std::ostringstream out;
    const int total = static_cast<int>(reports.size());
    int passed = 0;
    for (const auto& r : reports)
        if (r.failures.empty()) ++passed;
    const double score = run_score(reports) * 10.0;

    out << "# Harness Scorecard — " << meta.model << " (run "
        << meta.run_id << ")\n\n";
    out << "model score: " << static_cast<int>(score) << "/1000"
        << "  pass: " << passed << "/" << total << "\n\n";

    // ------------------------------------------------------------------
    // 1. Dimension aggregates with verdicts
    // ------------------------------------------------------------------
    RunAgg a;
    for (const auto& r : reports) agg_add(a, r);
    const double per_scenario =
        a.n > 0 ? static_cast<double>(a.steps) / a.n : 0.0;

    out << "## 1. Dimensions (run-wide, " << a.n << " scenarios)\n\n";

    // Loop control dimension.
    out << "### Loop control\n";
    out << "  breakouts fired:          " << a.breakouts << "/" << a.n
        << " runs (loop detection broke an identical-call loop)\n";
    out << "  hard stops:               " << a.hard_stops << "\n";
    out << "  recoveries (any kind):    " << a.recoveries
        << "  | steers: " << a.steers
        << "  | steers effective: " << a.steers_effective << "\n";
    out << "  retries:                  " << a.retries << "\n";
    out << "  verdict:                  "
        << (a.breakouts == 0 && a.hard_stops == 0 && a.recoveries == 0
                ? "no loop events observed — the engine never had to "
                  "intervene (or loop detection is off in live mode)"
                : "engine intervened; see per-scenario detail below")
        << "\n\n";

    // Tool economy dimension.
    out << "### Tool economy\n";
    out << "  total tool calls:         " << a.calls
        << "  | steps: " << a.steps
        << " (mean " << per_scenario << "/scenario)\n";
    out << "  wasted (off-plan):        " << a.wasted << " ("
        << (a.n > 0 ? static_cast<double>(a.wasted) / a.n : 0.0)
        << "/scenario)\n";
    out << "  redundant (identical):    " << a.redundant << "\n";
    out << "  tool failures:            " << a.failures << "\n";
    out << "  tool denied:              " << a.denied << "\n";
    out << "  mean bullseye:            "
        << (a.n > 0 ? a.bullseye_sum / a.n : 0.0) << "\n";
    out << "  verdict:                  "
        << (a.wasted > a.calls / 3
                ? "wasted calls dominate — the model surveys/repeats "
                  "instead of executing (tool economy is the primary loss)"
                : "tool economy within reason; failures/waste are localized")
        << "\n\n";

    // Planning dimension.
    out << "### Planning & adherence\n";
    out << "  replan_adapted runs:      " << a.replans << "/" << a.n << "\n";
    out << "  dependency violations:    " << a.dep_violations << "\n";
    out << "  mean plan_adherence:      "
        << (a.n > 0 ? a.adherence_sum / a.n : 0.0) << "\n";
    out << "  verdict:                  "
        << (a.dep_violations > 0
                ? "dependency-order violations detected — planning skips "
                  "required reads before writes"
                : "no dependency violations; adherence is order-clean")
        << "\n\n";

    // Performance dimension.
    out << "### Performance\n";
    out << "  mean wall:                " << (a.n > 0 ? a.wall_sum / a.n : 0)
        << " ms/scenario\n";
    out << "  mean tps:                 "
        << (a.tps_n > 0 ? a.tps_sum / a.tps_n : -1.0) << "\n";
    out << "  hard stops:               " << a.hard_stops << "\n\n";

    // ------------------------------------------------------------------
    // 2. Per-suite matrix
    // ------------------------------------------------------------------
    std::map<std::string, RunAgg> by_suite;
    for (const auto& r : reports) agg_add(by_suite[r.suite], r);
    out << "## 2. Per-suite\n\n";
    out << "suite                       scen  pass  mean  wasted  redund  "
           "fail  wall_ms\n";
    out << std::string(74, '-') << "\n";
    for (const auto& [suite, sa] : by_suite) {
        const double mean = sa.n > 0 ? sa.score_sum / sa.n : 0.0;
        out << std::left << std::setw(28) << suite << std::right
            << std::setw(6) << sa.n << std::setw(6) << sa.pass
            << std::setw(7) << static_cast<int>(mean) << std::setw(9)
            << sa.wasted << std::setw(9) << sa.redundant << std::setw(6)
            << sa.failures << std::setw(9)
            << (sa.n > 0 ? sa.wall_sum / sa.n : 0) << "\n";
    }
    out << "\n";

    // ------------------------------------------------------------------
    // 3. Failed-scenario diagnosis
    // ------------------------------------------------------------------
    out << "## 3. Failed scenarios (why, and which dimension)\n\n";
    int shown = 0;
    for (const auto& r : reports) {
        if (r.failures.empty()) continue;
        if (++shown > 12) {
            out << "  ... " << (passed < total ? total - passed - 12 : 0)
                << " more failures; run `amber-bench report` for full "
                   "details\n";
            break;
        }
        out << "### " << r.name << "  (score " << static_cast<int>(r.score.total)
            << ", " << r.suite << ")\n";
        for (const auto& f : r.failures) out << "  failure: " << f << "\n";
        out << "  steps=" << r.kpi.steps
            << " calls=" << r.kpi.tool_calls
            << " wasted=" << r.kpi.wasted
            << " redundant=" << r.kpi.redundant
            << " tool_failures=" << r.kpi.tool_failures
            << " denied=" << r.kpi.tool_denied;
        if (r.kpi.hard_stop) out << " HARD_STOP";
        if (r.replan_adapted) out << " replan=adapted";
        if (r.dependency_violation) out << " DEP_VIOLATION";
        if (r.breakout_latency > 0)
            out << " breakout@" << r.breakout_latency;
        if (r.steer_effective) out << " steer=effective";
        out << "\n";
        // The tool trace: what actually executed.
        if (!r.tool_details.empty()) {
            out << "  trace:";
            for (const auto& d : r.tool_details)
                out << " " << d.name << "[" << d.status
                    << (d.error.empty() ? "" : ":" + d.error) << "]";
            out << "\n";
        }
        out << "\n";
    }
    if (shown == 0) out << "  no failures — clean run\n";

    // ------------------------------------------------------------------
    // 4. Signals
    // ------------------------------------------------------------------
    out << "## 4. Signals (what to fix next)\n\n";
    if (a.wasted > 0)
        out << "- **Tool economy**: " << a.wasted
            << " wasted calls (" << (a.n > 0 ? a.wasted / a.n : 0)
            << "/scenario). The model surveys or repeats instead of "
               "executing. Candidates: tool-selection steering in the "
               "prompt, or the agentic-efficiency weighting.\n";
    if (a.redundant > 0)
        out << "- **Redundancy**: " << a.redundant
            << " identical calls repeated. Duplicate-call detection is "
               "worth enabling in live mode.\n";
    if (a.failures > 0)
        out << "- **Tool failures**: " << a.failures
            << " failed calls. Check the per-call `tool_details` error text "
               "for the dominant failure reason.\n";
    if (a.dep_violations > 0)
        out << "- **Dependency violations**: " << a.dep_violations
            << " runs wrote before reading the dependency. Planning "
               "adherence needs prompt-level ordering guidance.\n";
    if (a.breakouts == 0 && a.hard_stops == 0)
        out << "- **Loop detection never fired**: no breakouts observed. "
               "Verify `detection_loop` is enabled in live mode — the "
               "hermetic probes prove the engine CAN break loops; the live "
               "config may have it off.\n";
    if (a.recoveries > 0 && a.steers_effective == 0)
        out << "- **Steers ineffective**: " << a.recoveries
            << " recovery events but none led to completion. The steer "
               "wording may not reach the model.\n";
    out << "\n";
    return out.str();
}

} // namespace bench
