
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
std::map<std::string, std::vector<double>>
scenario_totals(const std::vector<std::vector<ScenarioReport>>& population) {
    std::map<std::string, std::vector<double>> totals;
    for (const auto& run : population)
        for (const auto& r : run)
            totals[r.name].push_back(r.score.total);
    return totals;
}
} // namespace

std::map<std::string, double>
discrimination_weights(const std::vector<std::vector<ScenarioReport>>& population) {
    std::map<std::string, double> weights;
    const auto totals = scenario_totals(population);
    for (const auto& [name, scores] : totals)
        weights[name] = stddev(scores);
    return weights;
}

double run_score_discriminative(const std::vector<ScenarioReport>& reports,
                                const std::map<std::string, double>& weights) noexcept {
    if (weights.empty())
        return run_score(reports);
    double weighted = 0.0;
    double weight_sum = 0.0;
    for (const auto& r : reports) {
        const auto it = weights.find(r.name);
        if (it == weights.end())
            continue; // unknown scenario: no signal
        const double w = it->second * static_cast<double>(r.difficulty);
        weighted += w * r.score.total;
        weight_sum += w;
    }
    return weight_sum > 0.0 ? weighted / weight_sum : run_score(reports);
}

double median(std::vector<double> values) noexcept {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[(n / 2) - 1] + values[n / 2]) / 2.0;
}

double stddev(const std::vector<double>& values) noexcept {
    if (values.size() < 2)
        return 0.0;
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double sq = 0.0;
    for (const double v : values)
        sq += (v - mean) * (v - mean);
    return std::sqrt(sq / (values.size() - 1));
}

namespace {
// Run whose total sits closest to the median — the representative report.
size_t median_representative(const std::vector<ScenarioReport>& runs, double score_median) {
    size_t best = 0;
    auto closest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < runs.size(); ++i) {
        const double d = std::abs(runs[i].score.total - score_median);
        if (d < closest) {
            closest = d;
            best = i;
        }
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
    if (runs.empty())
        return {};
    ScenarioReport agg = runs.front();
    agg.repeat_n = static_cast<int>(runs.size());
    agg.repeat_scores.clear();
    for (const auto& r : runs)
        agg.repeat_scores.push_back(r.score.total);
    if (runs.size() == 1) {
        // Single runs still carry the metadata: median = the score itself.
        agg.score_median = agg.score.total;
        return agg;
    }
    agg.score_median = median(agg.repeat_scores);
    agg.score_stddev = stddev(agg.repeat_scores);
    copy_representative_details(agg, runs[median_representative(runs, agg.score_median)]);
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
double bootstrap_weighted_score(const std::vector<ScenarioReport>& reports, uint32_t& rng,
                                const std::map<std::string, double>& weights) {
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
        if (it != weights.end())
            w *= it->second;
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
                      const std::map<std::string, double>& weights) {
    for (const auto& r : reports)
        if (r.repeat_n < 2 || r.repeat_scores.size() < 2)
            return -1.0;
    uint32_t rng = 0xC0FFEEu;
    std::vector<double> boot;
    boot.reserve(200);
    for (int i = 0; i < 200; ++i)
        boot.push_back(bootstrap_weighted_score(reports, rng, weights));
    return 1.96 * stddev(boot);
}

bool resolvable(double ci_a, double score_a, double ci_b, double score_b) noexcept {
    const double gap = std::abs(score_a - score_b);
    // ci_a/ci_b are already 95% intervals; the combined interval of the
    // difference is their root-sum-square (independent runs).
    const double combined = std::sqrt((ci_a * ci_a) + (ci_b * ci_b));
    return gap > combined;
}

namespace {

// One line saying which plugins produced the run. A result with no plugin list
// says so rather than implying a default set: either it predates the field or
// the run never got as far as recording it, and both mean the same thing here.
std::string plugins_line(const RunMeta& meta) {
    if (meta.plugins.empty())
        return "plugins: not recorded\n";
    std::string off;
    for (const auto& p : meta.plugins) {
        if (!p.enabled) {
            if (!off.empty())
                off += ", ";
            off += p.id;
        }
    }
    std::string line = "plugins: " + std::to_string(meta.plugins.size()) + " registered";
    line += off.empty() ? ", all on" : ", off: " + off;
    return line + "\n";
}

} // namespace

// The 95% CI suffix for a run's score, or the single-run note.
std::string score_ci_suffix(const std::vector<ScenarioReport>& reports) {
    const double ci = model_score_ci(reports); // difficulty-weighted for single runs
    if (ci <= 0.0)
        return "  (single-run: no CI)";
    std::ostringstream s;
    s << std::fixed << std::setprecision(1) << (ci * 10.0);
    return "  ±" + s.str() + " (95% CI)";
}

// The median/σ suffix for a repeated scenario, empty for a single run.
std::string repeat_suffix(const ScenarioReport& r) {
    if (r.repeat_n <= 1)
        return "";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << " (median " << r.score_median << ", σ "
       << r.score_stddev << ")";
    return ss.str();
}

// One scenario's summary line.
void render_scenario_line(std::ostream& out, const ScenarioReport& r) {
    out << (r.kpi.success ? "PASS" : "FAIL") << "  " << r.name << " (" << r.suite << ")"
        << "  score=" << static_cast<int>(r.score.total) << repeat_suffix(r) << " d" << r.difficulty
        << " bullseye=" << r.kpi.bullseye << " steps=" << r.kpi.steps
        << " cd=" << r.kpi.bash_cd_prefix << " wasted=" << r.kpi.wasted << " wall=" << r.kpi.wall_ms
        << "ms" << " retries=" << r.kpi.retries << " recoveries=" << r.kpi.recoveries;
    if (r.templated)
        out << " artifact=" << r.kpi.artifact_score
            << " compile=" << (r.kpi.compile_ok ? "ok" : "FAIL")
            << " behavior=" << (r.kpi.behavior_equivalent ? "eq" : "diff");
    out << "\n";
}

// A failed scenario's detail: its failures, a capped tool-call trace, and the
// truncated final answer.
void render_scenario_failures(std::ostream& out, const ScenarioReport& r) {
    if (r.failures.empty())
        return;
    for (const auto& f : r.failures)
        out << "    - " << f << "\n";
    const size_t cap = std::min<size_t>(r.tool_calls.size(), 12);
    for (size_t i = 0; i < cap; ++i) {
        std::string a = r.tool_calls[i].second;
        if (a.size() > 80) {
            a.resize(77);
            a += "...";
        }
        out << "    call: " << r.tool_calls[i].first << " " << a << "\n";
    }
    if (r.tool_calls.size() > cap)
        out << "    ... " << (r.tool_calls.size() - cap) << " more calls (full trace in JSON)\n";
    if (r.final_text.empty())
        return;
    std::string t = r.final_text;
    if (t.size() > 140) {
        t.resize(137);
        t += "...";
    }
    out << "    final: " << t << "\n";
}

std::string render_text(const std::vector<ScenarioReport>& reports, const RunMeta& meta) {
    std::ostringstream out;
    out << "amber-bench run " << meta.run_id << " [" << meta.mode << ", "
        << (meta.profile.empty() ? "default" : meta.profile) << ", " << meta.model << "]\n";
    out << "engine " << meta.engine_version << " @ " << meta.timestamp << "\n";
    out << plugins_line(meta);
    out << "model score: " << (run_score(reports) * 10.0) << "/1000" << score_ci_suffix(reports)
        << "\n";
    int passed = 0;
    for (const auto& r : reports) {
        if (r.kpi.success)
            ++passed;
        render_scenario_line(out, r);
        render_scenario_failures(out, r);
    }
    out << passed << "/" << reports.size() << " scenarios passed\n";
    return out.str();
}

// The run metadata, including the plugin set that produced it. Plugins are
// absent from older results, which is itself information: they predate plugin
// state being recorded.
agent::json json_meta(const RunMeta& meta) {
    agent::json out = agent::json::object();
    out["run_id"] = meta.run_id;
    out["mode"] = meta.mode;
    out["profile"] = meta.profile;
    out["model"] = meta.model;
    out["engine_version"] = meta.engine_version;
    out["timestamp"] = meta.timestamp;
    out["reasoning"] = meta.reasoning;
    agent::json plugs = agent::json::array();
    for (const auto& p : meta.plugins) {
        agent::json jp = agent::json::object();
        jp["id"] = p.id;
        jp["enabled"] = p.enabled;
        jp["tier"] = p.tier;
        plugs.push_back(std::move(jp));
    }
    out["plugins"] = std::move(plugs);
    return out;
}

// The KPI block: what the engine measured.
agent::json json_kpi(const ScenarioReport& r) {
    agent::json j = agent::json::object();
    j["success"] = r.kpi.success;
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
    return j;
}

// The agentic-economy block: how closely the run followed a plan. These stay
// flat, `agentic_`-prefixed, because the JSON is a stored format.
void add_agentic_fields(agent::json& j, const ScenarioReport& r) {
    j["agentic_has_plan"] = r.agentic.has_plan;
    j["agentic_plan_tools"] = r.agentic.plan_tools;
    j["agentic_deviation"] = r.agentic.plan_deviation;
    j["agentic_ratio"] = r.agentic.plan_ratio;
    j["agentic_efficiency_pct"] = r.agentic.efficiency_pct;
    j["agentic_score"] = r.agentic.score;
    j["agentic_plan_by_tool"] = r.agentic.plan_by_tool;
    j["agentic_actual_by_tool"] = r.agentic.actual_by_tool;
}

// The recorded tool calls, with their arguments.
agent::json json_tool_calls(const ScenarioReport& r) {
    agent::json calls = agent::json::array();
    for (const auto& tc : r.tool_calls) {
        agent::json cj;
        cj["tool"] = tc.first;
        cj["args"] = tc.second;
        calls.push_back(std::move(cj));
    }
    return calls;
}

// Per-call telemetry (BENCH-11): the post-mortem story.
agent::json json_tool_details(const ScenarioReport& r) {
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
    return details;
}

// One scenario's JSON object.
agent::json json_scenario(const ScenarioReport& r) {
    agent::json j = json_kpi(r);
    j["name"] = r.name;
    j["suite"] = r.suite;
    j["difficulty"] = r.difficulty;
    j["score"] = r.score.total;
    j["repeat_n"] = r.repeat_n;
    j["score_median"] = r.score_median;
    j["score_stddev"] = r.score_stddev;
    if (!r.repeat_scores.empty())
        j["repeat_scores"] = r.repeat_scores;
    j["score_correctness"] = r.score.correctness;
    j["score_efficiency"] = r.score.efficiency;
    j["score_robustness"] = r.score.robustness;
    j["score_adherence"] = r.score.adherence;
    add_agentic_fields(j, r);
    j["templated"] = r.templated;
    j["final_text"] = r.final_text;
    j["tool_calls"] = json_tool_calls(r);
    j["tool_details"] = json_tool_details(r);
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
    return j;
}

std::string render_json(const std::vector<ScenarioReport>& reports, const RunMeta& meta) {
    agent::json out = json_meta(meta);
    agent::json arr = agent::json::array();
    for (const auto& r : reports)
        arr.push_back(json_scenario(r));
    out["scenarios"] = std::move(arr);
    out["model_score"] = run_score(reports) * 10.0;
    out["model_score_ci"] = model_score_ci(reports) * 10.0;
    return out.dump(2) + "\n";
}

// Run-wide economy/agentic totals for the markdown profile tables.
struct EconomyTotals {
    int tools = 0;
    int failures = 0;
    int denied = 0;
    int redundant = 0;
    int retries = 0;
    int cd_prefix = 0;
    long wall_ms = 0;
    int plan_total = 0;
    int deviation = 0;
    int plan_runs = 0;
    double agentic_sum = 0.0;
};

EconomyTotals economy_totals(const std::vector<ScenarioReport>& reports) {
    EconomyTotals t;
    for (const auto& r : reports) {
        t.tools += r.kpi.tool_calls;
        t.failures += r.kpi.tool_failures;
        t.denied += r.kpi.tool_denied;
        t.redundant += r.kpi.redundant;
        t.retries += r.kpi.retries;
        t.cd_prefix += r.kpi.bash_cd_prefix;
        t.wall_ms += r.kpi.wall_ms;
        if (!r.agentic.has_plan)
            continue;
        t.plan_total += r.agentic.plan_tools;
        t.deviation += r.agentic.plan_deviation;
        t.agentic_sum += r.agentic.score;
        ++t.plan_runs;
    }
    return t;
}

void render_md_header(std::ostream& out, const std::vector<ScenarioReport>& reports,
                      const RunMeta& meta) {
    out << "## " << meta.model << "\n\n";
    out << "- run: `" << meta.run_id << "` [" << meta.mode << ", engine " << meta.engine_version
        << ", reasoning " << (meta.reasoning.empty() ? "default" : meta.reasoning) << "]\n";
    out << "- **model score: " << static_cast<int>(run_score(reports) * 10.0) << "/1000** ("
        << reports.size() << " scenarios)\n\n";
}

void render_md_scenario_table(std::ostream& out, const std::vector<ScenarioReport>& reports) {
    out << "| scenario | d | score | bullseye | steps | wasted | wall (s) | artifact |\n";
    out << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& r : reports) {
        out << "| " << r.name << " | " << r.difficulty << " | " << static_cast<int>(r.score.total)
            << " | " << r.kpi.bullseye << " | " << r.kpi.steps << " | " << r.kpi.wasted << " | "
            << (r.kpi.wall_ms / 1000.0) << " | ";
        out << " | ";
        if (r.templated)
            out << r.kpi.artifact_score;
        else
            out << "-";
        out << " |\n";
    }
}

void render_md_failures(std::ostream& out, const std::vector<ScenarioReport>& reports) {
    out << "\n### Failures\n\n";
    bool any = false;
    for (const auto& r : reports) {
        if (r.kpi.success)
            continue;
        any = true;
        out << "- **" << r.name << "** (" << static_cast<int>(r.score.total) << "/100): ";
        for (size_t i = 0; i < r.failures.size(); ++i) {
            if (i)
                out << "; ";
            out << r.failures[i];
        }
        out << "\n";
    }
    if (!any)
        out << "none\n";
}

// One "total | per scenario" row of the economy table.
void md_economy_row(std::ostream& out, const std::string& label, double total, int n) {
    out << "| " << label << " | " << total << " | " << (n ? total / n : 0.0) << " |\n";
}

void render_md_economy_table(std::ostream& out, const EconomyTotals& t, int n) {
    out << "| metric | total | per scenario |\n|---|---|---|\n";
    md_economy_row(out, "tool calls", t.tools, n);
    md_economy_row(out, "bash cd-prefix calls", t.cd_prefix, n);
    md_economy_row(out, "tool failures", t.failures, n);
    md_economy_row(out, "tool denials", t.denied, n);
    md_economy_row(out, "redundant calls", t.redundant, n);
    md_economy_row(out, "LLM retries", t.retries, n);
    md_economy_row(out, "wall time (s)", t.wall_ms / 1000.0, n);
}

// Only the runs that had a plan contribute to the adherence tables.
void render_md_plan_adherence(std::ostream& out, const std::vector<ScenarioReport>& reports,
                              const EconomyTotals& t) {
    if (t.plan_runs <= 0)
        return;
    out << "\n**Plan adherence** (optimal tool plan vs actual):\n\n";
    out << "| metric | value |\n|---|---|\n";
    out << "| scenarios with a plan | " << t.plan_runs << " |\n";
    out << "| optimal tool calls (sum) | " << t.plan_total << " |\n";
    out << "| actual tool calls | " << t.tools << " |\n";
    out << "| total deviation (extra calls) | " << t.deviation << " |\n";
    const double eff = t.tools > 0 ? (100.0 * t.plan_total / t.tools) : 0.0;
    out << "| plan efficiency | " << static_cast<int>(eff > 100.0 ? 100.0 : eff) << "/100 |\n";
    out << "| agentic score (mean plan adherence) | "
        << static_cast<int>(t.agentic_sum / t.plan_runs) << "/100 |\n";

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
        const double tool_eff = act > 0 ? (100.0 * p.second / act) : 0.0;
        out << "| " << p.first << " | " << p.second << " | " << act << " | " << (act - p.second)
            << " | " << static_cast<int>(tool_eff > 100.0 ? 100.0 : tool_eff) << " |\n";
    }
    for (const auto& a : actual_mix) {
        if (plan_mix.count(a.first))
            continue;
        out << "| " << a.first << " | 0 | " << a.second << " | " << a.second << " | 0 |\n";
    }
}

std::string render_markdown(const std::vector<ScenarioReport>& reports, const RunMeta& meta) {
    std::ostringstream out;
    render_md_header(out, reports, meta);
    render_md_scenario_table(out, reports);
    render_md_failures(out, reports);
    out << "\n### Agentic profile\n\n";
    const EconomyTotals t = economy_totals(reports);
    render_md_economy_table(out, t, static_cast<int>(reports.size()));
    render_md_plan_adherence(out, reports, t);
    return out.str();
}

// The population's discrimination weights: with several models, participation
// trophies weigh ~0 and separators dominate the deltas (BENCH-02).
std::map<std::string, double>
comparison_weights(const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs) {
    std::vector<std::vector<ScenarioReport>> population;
    population.reserve(runs.size());
    for (const auto& run : runs)
        population.push_back(run.second);
    return discrimination_weights(population);
}

// One line per model, with its CI when there is repeat data.
void render_cmp_score_lines(
    std::ostream& out, const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs,
    const std::map<std::string, double>& weights) {
    for (const auto& run : runs) {
        out << "- **" << run.first.model
            << "**: " << static_cast<int>(run_score_discriminative(run.second, weights) * 10.0)
            << "/1000";
        const double ci = model_score_ci(run.second, weights);
        if (ci > 0.0) {
            std::ostringstream ci_s;
            ci_s << std::fixed << std::setprecision(1) << (ci * 10.0);
            out << " ±" << ci_s.str();
        }
        out << "\n";
    }
}

// The resolution rule: differences within the combined CI are noise.
void render_cmp_resolution(std::ostream& out,
                           const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs,
                           const std::map<std::string, double>& weights) {
    if (runs.size() < 2)
        return;
    out << "\n**Resolution** (a gap is a finding only when it exceeds the "
           "combined 95% CI):\n\n";
    for (size_t i = 0; i < runs.size(); ++i)
        for (size_t j = i + 1; j < runs.size(); ++j) {
            const double sa = run_score_discriminative(runs[i].second, weights);
            const double sb = run_score_discriminative(runs[j].second, weights);
            const double ca = model_score_ci(runs[i].second, weights);
            const double cb = model_score_ci(runs[j].second, weights);
            const std::string verdict =
                (ca < 0.0 || cb < 0.0)
                    ? "insufficient repeat data (run --repeat)"
                    : (resolvable(ca, sa, cb, sb) ? "**resolvable**" : "within noise");
            out << "- " << runs[i].first.model << " vs " << runs[j].first.model
                << ": Δ=" << static_cast<int>(std::abs(sa - sb) * 10.0) << " → " << verdict << "\n";
        }
}

// One model's economy row. The plan percentage is computed over the runs that
// HAD a plan, not over every tool call.
struct ComparisonRow {
    int agentic = 0;
    int plan_pct = 0;
    int tools = 0;
    int fail_pct = 0;
    int red_pct = 0;
    int steps = 0;
    long wall_ms = 0;
};

ComparisonRow comparison_row(const std::vector<ScenarioReport>& reports) {
    ComparisonRow row;
    int failures = 0;
    int redundant = 0;
    int plan_tools = 0;
    int plan_calls = 0;
    double ag_sum = 0.0;
    int ag_n = 0;
    for (const auto& r : reports) {
        row.tools += r.kpi.tool_calls;
        row.steps += r.kpi.steps;
        row.wall_ms += r.kpi.wall_ms;
        failures += r.kpi.tool_failures;
        redundant += r.kpi.redundant;
        if (!r.agentic.has_plan)
            continue;
        ag_sum += r.agentic.score;
        plan_tools += r.agentic.plan_tools;
        plan_calls += r.kpi.tool_calls;
        ++ag_n;
    }
    row.agentic = ag_n ? static_cast<int>(ag_sum / ag_n) : 0;
    row.fail_pct = row.tools > 0 ? (100 * failures / row.tools) : 0;
    row.red_pct = row.tools > 0 ? (100 * redundant / row.tools) : 0;
    double plan_pct = plan_calls > 0 ? (100.0 * plan_tools / plan_calls) : 0.0;
    if (plan_pct > 100.0)
        plan_pct = 100.0;
    row.plan_pct = static_cast<int>(plan_pct);
    return row;
}

void render_cmp_model_table(
    std::ostream& out, const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs,
    const std::map<std::string, double>& weights) {
    out << "\n| model | score | agentic | plan % | tools | fail % | redun % | "
           "steps | wall (s) |\n";
    out << "|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& run : runs) {
        const ComparisonRow row = comparison_row(run.second);
        out << "| " << run.first.model << " | "
            << static_cast<int>(run_score_discriminative(run.second, weights) * 10.0) << " | "
            << row.agentic << " | " << row.plan_pct << " | " << row.tools << " | " << row.fail_pct
            << " | " << row.red_pct << " | " << row.steps << " | " << (row.wall_ms / 1000.0)
            << " |\n";
    }
}

// The per-scenario matrix: one row per scenario, one column per model.
void render_cmp_scenario_matrix(
    std::ostream& out, const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs) {
    out << "\n| scenario |";
    for (const auto& run : runs)
        out << " " << run.first.model << " |";
    out << "\n|---|";
    for (size_t i = 0; i < runs.size(); ++i)
        out << "---|";
    out << "\n";
    const size_t n = runs[0].second.size();
    for (size_t i = 0; i < n; ++i) {
        out << "| " << runs[0].second[i].name << " |";
        for (const auto& run : runs) {
            if (i < run.second.size())
                out << " " << static_cast<int>(run.second[i].score.total) << " |";
            else
                out << " - |";
        }
        out << "\n";
    }
}

std::string render_markdown_comparison(
    const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs) {
    if (runs.empty())
        return "";
    std::ostringstream out;
    out << "# Benchmark: harness score by model\n\n";
    const std::map<std::string, double> weights = comparison_weights(runs);
    render_cmp_score_lines(out, runs, weights);
    render_cmp_resolution(out, runs, weights);
    render_cmp_model_table(out, runs, weights);
    render_cmp_scenario_matrix(out, runs);
    out << "\n---\n\n";
    for (const auto& run : runs)
        out << render_markdown(run.second, run.first) << "\n";
    return out.str();
}

// The run metadata, plus the plugin set that produced it.
void parse_run_meta(const agent::json& j, RunMeta& meta) {
    meta.run_id = j.value("run_id", "");
    meta.mode = j.value("mode", "");
    meta.profile = j.value("profile", "");
    meta.model = j.value("model", "");
    meta.engine_version = j.value("engine_version", "");
    meta.timestamp = j.value("timestamp", "");
    meta.reasoning = j.value("reasoning", "");
    if (!j.contains("plugins"))
        return;
    for (const auto& jp : j["plugins"]) {
        PluginRecord p;
        p.id = jp.value("id", "");
        p.enabled = jp.value("enabled", false);
        p.tier = jp.value("tier", "");
        meta.plugins.push_back(std::move(p));
    }
}

// A {tool: count} object, skipping anything that is not an integer.
void parse_tool_counts(const agent::json& e, const char* key, std::map<std::string, int>& out) {
    if (!e.contains(key) || !e[key].is_object())
        return;
    for (auto it = e[key].begin(); it != e[key].end(); ++it)
        if (it.value().is_number_integer())
            out[it.key()] = it.value().get<int>();
}

void parse_repeat_scores(const agent::json& e, ScenarioReport& rep) {
    if (!e.contains("repeat_scores") || !e["repeat_scores"].is_array())
        return;
    for (const auto& v : e["repeat_scores"])
        if (v.is_number())
            rep.repeat_scores.push_back(v.get<double>());
}

// Failures + per-call telemetry must survive the round trip, or a stored report
// cannot be diagnosed (the scorecard's section 3).
void parse_failures_and_telemetry(const agent::json& e, ScenarioReport& rep) {
    if (e.contains("failures") && e["failures"].is_array())
        for (const auto& f : e["failures"])
            if (f.is_string())
                rep.failures.push_back(f.get<std::string>());
    if (!e.contains("tool_details") || !e["tool_details"].is_array())
        return;
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

// One scenario's fields.
void parse_scenario(const agent::json& e, ScenarioReport& rep) {
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
    parse_repeat_scores(e, rep);
    rep.templated = e.value("templated", false);
    rep.agentic.has_plan = e.value("agentic_has_plan", false);
    rep.agentic.plan_tools = e.value("agentic_plan_tools", 0);
    rep.agentic.plan_deviation = e.value("agentic_deviation", 0);
    rep.agentic.plan_ratio = e.value("agentic_ratio", 0.0);
    rep.agentic.efficiency_pct = e.value("agentic_efficiency_pct", 0.0);
    rep.agentic.score = e.value("agentic_score", 0.0);
    parse_tool_counts(e, "agentic_plan_by_tool", rep.agentic.plan_by_tool);
    parse_tool_counts(e, "agentic_actual_by_tool", rep.agentic.actual_by_tool);
    parse_failures_and_telemetry(e, rep);
}

bool parse_report_json(const agent::json& j, RunMeta& meta, std::vector<ScenarioReport>& reports) {
    if (!j.contains("scenarios") || !j["scenarios"].is_array())
        return false;
    parse_run_meta(j, meta);
    for (const auto& e : j["scenarios"]) {
        ScenarioReport rep;
        parse_scenario(e, rep);
        reports.push_back(std::move(rep));
    }
    return true;
}

double reference_score(const std::vector<std::vector<ScenarioReport>>& population,
                       size_t reference) noexcept {
    if (reference >= population.size())
        return 0.0;
    return run_score(population[reference]);
}

std::vector<double> anchor_weights(const std::vector<std::vector<ScenarioReport>>& population,
                                   size_t reference, double target) {
    std::vector<double> w;
    if (reference >= population.size())
        return w;
    for (const auto& r : population[reference]) {
        const double s = r.score.total;
        if (std::abs(s - target) < 0.001) {
            w.push_back(1.0);
            continue;
        }
        double a = target / std::abs(s - target);
        if (a < 0.25)
            a = 0.25;
        if (a > 200.0)
            a = 200.0;
        w.push_back(a);
    }
    return w;
}

std::vector<int> suggest_difficulties(const std::vector<std::vector<ScenarioReport>>& population,
                                      size_t reference, double target) {
    const std::vector<double> w = anchor_weights(population, reference, target);
    std::vector<int> out;
    out.reserve(w.size());
    for (const double a : w) {
        // Map the continuous weight onto the 1..6 ladder, monotonic in the
        // reference score (solved well -> low weight -> low difficulty).
        int d = static_cast<int>(std::lround(6.0 * a / (a + 1.0)));
        if (d < 1)
            d = 1;
        if (d > 6)
            d = 6;
        out.push_back(d);
    }
    return out;
}

double reference_anchor_deviation(const std::vector<std::vector<ScenarioReport>>& population,
                                  size_t reference, double target) noexcept {
    return std::abs(reference_score(population, reference) - target);
}

double headroom(const std::vector<std::vector<ScenarioReport>>& population) noexcept {
    double best = 0.0;
    for (const auto& run : population) {
        const double s = run_score(run);
        if (s > best)
            best = s;
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
    if (r.failures.empty())
        ++a.pass;
    a.wasted += r.kpi.wasted;
    a.redundant += r.kpi.redundant;
    a.failures += r.kpi.tool_failures;
    a.denied += r.kpi.tool_denied;
    a.retries += r.kpi.retries;
    a.recoveries += r.kpi.recoveries;
    a.steers += r.kpi.steers;
    if (r.kpi.hard_stop)
        ++a.hard_stops;
    if (r.replan_adapted)
        ++a.replans;
    if (r.dependency_violation)
        ++a.dep_violations;
    if (r.breakout_latency > 0)
        ++a.breakouts;
    if (r.steer_effective)
        ++a.steers_effective;
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

// §1 Dimension aggregates with a verdict per dimension.
void render_loop_control(std::ostream& out, const RunAgg& a) {
    out << "### Loop control\n";
    out << "  breakouts fired:          " << a.breakouts << "/" << a.n
        << " runs (loop detection broke an identical-call loop)\n";
    out << "  hard stops:               " << a.hard_stops << "\n";
    out << "  recoveries (any kind):    " << a.recoveries << "  | steers: " << a.steers
        << "  | steers effective: " << a.steers_effective << "\n";
    out << "  retries:                  " << a.retries << "\n";
    out << "  verdict:                  "
        << (a.breakouts == 0 && a.hard_stops == 0 && a.recoveries == 0
                ? "no loop events observed — the engine never had to "
                  "intervene (or loop detection is off in live mode)"
                : "engine intervened; see per-scenario detail below")
        << "\n\n";
}

void render_tool_economy(std::ostream& out, const RunAgg& a) {
    const double per_scenario = a.n > 0 ? static_cast<double>(a.steps) / a.n : 0.0;
    out << "### Tool economy\n";
    out << "  total tool calls:         " << a.calls << "  | steps: " << a.steps << " (mean "
        << per_scenario << "/scenario)\n";
    out << "  wasted (off-plan):        " << a.wasted << " ("
        << (a.n > 0 ? static_cast<double>(a.wasted) / a.n : 0.0) << "/scenario)\n";
    out << "  redundant (identical):    " << a.redundant << "\n";
    out << "  tool failures:            " << a.failures << "\n";
    out << "  tool denied:              " << a.denied << "\n";
    out << "  mean bullseye:            " << (a.n > 0 ? a.bullseye_sum / a.n : 0.0) << "\n";
    out << "  verdict:                  "
        << (a.wasted > a.calls / 3 ? "wasted calls dominate — the model surveys/repeats "
                                     "instead of executing (tool economy is the primary loss)"
                                   : "tool economy within reason; failures/waste are localized")
        << "\n\n";
}

void render_planning(std::ostream& out, const RunAgg& a) {
    out << "### Planning & adherence\n";
    out << "  replan_adapted runs:      " << a.replans << "/" << a.n << "\n";
    out << "  dependency violations:    " << a.dep_violations << "\n";
    out << "  mean plan_adherence:      " << (a.n > 0 ? a.adherence_sum / a.n : 0.0) << "\n";
    out << "  verdict:                  "
        << (a.dep_violations > 0 ? "dependency-order violations detected — planning skips "
                                   "required reads before writes"
                                 : "no dependency violations; adherence is order-clean")
        << "\n\n";
}

void render_performance(std::ostream& out, const RunAgg& a) {
    out << "### Performance\n";
    out << "  mean wall:                " << (a.n > 0 ? a.wall_sum / a.n : 0) << " ms/scenario\n";
    out << "  mean tps:                 " << (a.tps_n > 0 ? a.tps_sum / a.tps_n : -1.0) << "\n";
    out << "  hard stops:               " << a.hard_stops << "\n\n";
}

void render_dimensions(std::ostream& out, const RunAgg& a) {
    out << "## 1. Dimensions (run-wide, " << a.n << " scenarios)\n\n";
    render_loop_control(out, a);
    render_tool_economy(out, a);
    render_planning(out, a);
    render_performance(out, a);
}

// §2 The per-suite matrix.
void render_per_suite(std::ostream& out, const std::vector<ScenarioReport>& reports) {
    std::map<std::string, RunAgg> by_suite;
    for (const auto& r : reports)
        agg_add(by_suite[r.suite], r);
    out << "## 2. Per-suite\n\n";
    out << "suite                       scen  pass  mean  wasted  redund  "
           "fail  wall_ms\n";
    out << std::string(74, '-') << "\n";
    for (const auto& [suite, sa] : by_suite) {
        const double mean = sa.n > 0 ? sa.score_sum / sa.n : 0.0;
        out << std::left << std::setw(28) << suite << std::right << std::setw(6) << sa.n
            << std::setw(6) << sa.pass << std::setw(7) << static_cast<int>(mean) << std::setw(9)
            << sa.wasted << std::setw(9) << sa.redundant << std::setw(6) << sa.failures
            << std::setw(9) << (sa.n > 0 ? sa.wall_sum / sa.n : 0) << "\n";
    }
    out << "\n";
}

// §3 Failed-scenario diagnosis, capped so one bad run cannot flood the card.
// The "steps=… calls=… wasted=…" line plus the flag suffixes.
void render_failure_kpi(std::ostream& out, const ScenarioReport& r) {
    out << "  steps=" << r.kpi.steps << " calls=" << r.kpi.tool_calls << " wasted=" << r.kpi.wasted
        << " redundant=" << r.kpi.redundant << " tool_failures=" << r.kpi.tool_failures
        << " denied=" << r.kpi.tool_denied;
    if (r.kpi.hard_stop)
        out << " HARD_STOP";
    if (r.replan_adapted)
        out << " replan=adapted";
    if (r.dependency_violation)
        out << " DEP_VIOLATION";
    if (r.breakout_latency > 0)
        out << " breakout@" << r.breakout_latency;
    if (r.steer_effective)
        out << " steer=effective";
    out << "\n";
}

// The tool trace: what actually executed.
void render_failure_trace(std::ostream& out, const ScenarioReport& r) {
    if (r.tool_details.empty())
        return;
    out << "  trace:";
    for (const auto& d : r.tool_details)
        out << " " << d.name << "[" << d.status << (d.error.empty() ? "" : ":" + d.error) << "]";
    out << "\n";
}

// One failed scenario: its heading, its failures, its KPI line and its trace.
void render_one_failure(std::ostream& out, const ScenarioReport& r) {
    out << "### " << r.name << "  (score " << static_cast<int>(r.score.total) << ", " << r.suite
        << ")\n";
    for (const auto& f : r.failures)
        out << "  failure: " << f << "\n";
    render_failure_kpi(out, r);
    render_failure_trace(out, r);
    out << "\n";
}

void render_failures(std::ostream& out, const std::vector<ScenarioReport>& reports, int passed,
                     int total) {
    out << "## 3. Failed scenarios (why, and which dimension)\n\n";
    int shown = 0;
    for (const auto& r : reports) {
        if (r.failures.empty())
            continue;
        if (++shown > 12) {
            out << "  ... " << (passed < total ? total - passed - 12 : 0)
                << " more failures; run `amber-bench report` for full details\n";
            break;
        }
        render_one_failure(out, r);
    }
    if (shown == 0)
        out << "  no failures — clean run\n";
}

// §4 What to fix next.
void render_signals(std::ostream& out, const RunAgg& a) {
    out << "## 4. Signals (what to fix next)\n\n";
    if (a.wasted > 0)
        out << "- **Tool economy**: " << a.wasted << " wasted calls ("
            << (a.n > 0 ? a.wasted / a.n : 0)
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
}

} // namespace

std::string render_scorecard(const std::vector<ScenarioReport>& reports, const RunMeta& meta) {
    std::ostringstream out;
    const int total = static_cast<int>(reports.size());
    int passed = 0;
    for (const auto& r : reports)
        if (r.failures.empty())
            ++passed;
    const double score = run_score(reports) * 10.0;

    out << "# Harness Scorecard — " << meta.model << " (run " << meta.run_id << ")\n\n";
    out << "model score: " << static_cast<int>(score) << "/1000" << "  pass: " << passed << "/"
        << total << "\n";
    out << plugins_line(meta) << "\n";

    RunAgg a;
    for (const auto& r : reports)
        agg_add(a, r);
    render_dimensions(out, a);
    render_per_suite(out, reports);
    render_failures(out, reports, passed, total);
    render_signals(out, a);
    return out.str();
}

} // namespace bench
