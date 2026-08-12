
#include "bench/kpi.h"

#include <algorithm>
#include <map>
#include <set>

namespace bench {

namespace {

} // namespace

Kpi compute_kpi(const EventStream& stream, const OracleResult& oracle,
                const ResourceMeter& meter, const TemplateResult& tmpl,
                const Checks& prompt_checks, const std::string& final_text,
                long wall_ms, long bullseye_at_ms) {
    Kpi k;
    k.bullseye = oracle.bullseye;
    k.arg_precision = oracle.arg_precision;
    k.tool_call_accuracy = oracle.total_calls > 0
                               ? static_cast<double>(oracle.on_oracle_calls) /
                                     static_cast<double>(oracle.total_calls)
                               : 1.0;
    k.steps = stream.iterations;
    k.compressions = stream.compressions;
    k.bash_cd_prefix = stream.bash_cd_prefix;
    k.tool_calls = static_cast<int>(stream.calls.size());
    for (const auto& c : stream.calls) {
        if (c.status == "error") {
            ++k.tool_failures;
            // Classify the failure by its most specific available signal.
            const char* reason = "error";
            for (const auto& t : stream.tools) {
                if (t.name == c.name && !t.error.empty()) {
                    reason = "error";
                    if (t.timeout) reason = "timeout";
                    break;
                }
            }
            ++k.failure_taxonomy[reason];
        }
        if (c.status == "denied") ++k.tool_denied;
    }
    k.wasted = oracle.wasted;
    k.redundant = oracle.redundant;
    k.retries = static_cast<int>(stream.retries.size());
    k.recoveries = static_cast<int>(stream.recoveries.size());
    for (const auto& r : stream.recoveries)
        if (r.kind == "steer") ++k.steers;
    k.hard_stop = stream.hard_stop;
    k.wall_ms = wall_ms;
    k.bullseye_at_ms = bullseye_at_ms;
    k.ttft_ms = stream.ttft_ms;
    double tps_sum = 0.0;
    int tps_n = 0;
    for (const auto& s : stream.stats) {
        if (s.tps >= 0) {
            tps_sum += s.tps;
            ++tps_n;
        }
    }
    k.tps_avg = tps_n > 0 ? tps_sum / tps_n : -1.0;
    k.prompt_tokens = stream.prompt_tokens;
    k.completion_tokens = stream.completion_tokens;
    k.baseline_rss_kb = meter.baseline_rss_kb();
    k.peak_rss_kb = meter.peak_rss_kb();
    k.cpu_ms = meter.cpu_ms();

    std::set<std::string> touched;
    for (const auto& e : stream.tools)
        if (e.name == "write" && e.args.is_object() &&
            e.args.contains("path") && e.args["path"].is_string())
            touched.insert(e.args["path"].get<std::string>());
    k.files_touched = static_cast<int>(touched.size());

    if (tmpl.tests_total > 0) {
        k.artifact_score =
            static_cast<double>(tmpl.tests_passed) /
            static_cast<double>(tmpl.tests_total);
        k.compile_ok = tmpl.compile_ok;
        k.behavior_equivalent = tmpl.behavior_equivalent;
        k.structure_checks = tmpl.structure_checks;
    } else {
        k.artifact_score = 1.0;
        k.compile_ok = true;
        k.behavior_equivalent = true;
        k.structure_checks = 1.0;
    }

    k.prompt_adherence = adherence(prompt_checks, final_text);
    k.success = oracle.success && !stream.hard_stop &&
                adherence(prompt_checks, final_text) == 1.0;
    return k;
}

bool kpi_success(const Kpi& k, const Scenario& s) noexcept {
    if (!k.success) return false;
    if (s.max_steps > 0 && k.steps > s.max_steps) return false;
    if (s.max_wall_ms > 0 && k.wall_ms > s.max_wall_ms) return false;
    return true;
}

namespace {

double clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

int expected_steps(const Scenario& s) noexcept {
    if (s.expected_steps > 0) return s.expected_steps;
    if (!s.oracle.empty()) return static_cast<int>(s.oracle.size());
    return 5;
}

double correctness_templated(const Kpi& k) noexcept;
double correctness_plain(const Kpi& k, const Scenario& s,
                         double checks_ratio) noexcept;

double correctness_score(const Kpi& k, const Scenario& s,
                         double checks_ratio) noexcept {
    return s.template_dir.empty()
               ? correctness_plain(k, s, checks_ratio)
               : correctness_templated(k);
}

double correctness_templated(const Kpi& k) noexcept {
    return 100.0 * (0.5 * k.bullseye + 0.2 * k.arg_precision +
                    0.3 * k.artifact_score);
}

double correctness_plain(const Kpi& k, const Scenario& s,
                         double checks_ratio) noexcept {
    // Argument fidelity joins bullseye; the checks_weight knob lets a
    // review scenario (no meaningful tool-step oracle) put its issue
    // checks at the center of correctness.
    const double cw = s.checks_weight;
    return 100.0 *
           ((1.0 - cw) * (0.75 * k.bullseye + 0.25 * k.arg_precision) +
            cw * checks_ratio);
}
double efficiency_score(const Kpi& k, const Scenario& s) noexcept {
    const int excess = std::max(0, k.steps - expected_steps(s));
    const double wasted_pen = s.template_dir.empty() ? 0.10 * k.wasted : 0.0;
    return 100.0 * clamp01(1.0 - (0.10 * excess) - wasted_pen -
                           (0.20 * k.redundant));
}

} // namespace

double total_score(const Score& sc, double agentic_score) noexcept {
    return (0.40 * sc.correctness) + (0.25 * sc.efficiency) +
           (0.20 * agentic_score) + (0.10 * sc.robustness) +
           (0.05 * sc.adherence);
}

Score compute_score(const Kpi& k, const Scenario& s, double checks_ratio,
                    int forbidden_calls, double agentic_score) noexcept {
    Score sc;
    sc.correctness = correctness_score(k, s, checks_ratio);
    sc.efficiency = efficiency_score(k, s);

    sc.robustness =
        100.0 * clamp01(1.0 - (0.30 * k.retries) - (0.50 * k.recoveries) -
                        (k.hard_stop ? 1.0 : 0.0));

    sc.adherence =
        100.0 * clamp01(k.prompt_adherence - (0.25 * forbidden_calls));

    sc.total = total_score(sc, agentic_score);
    // A failed scenario (oracle miss, failed checks, budget breach, hard
    // stop) keeps its honest partial credit capped at 60 — near-miss
    // failures still cost, but are never silently halved from an inflated
    // total (that made pass/fail counts and scores diverge).
    if (!k.success) sc.total = std::min(sc.total, 60.0);
    return sc;
}

namespace {
// Tool-count plan: the scenario's declared optimal_plan, or the oracle's
// tool mix when absent. Empty when neither provides a baseline.
std::map<std::string, int> tool_plan(const Scenario& s) {
    std::map<std::string, int> plan;
    if (s.optimal_plan.is_object() && !s.optimal_plan.empty()) {
        for (auto it = s.optimal_plan.begin(); it != s.optimal_plan.end(); ++it)
            if (it.value().is_number_integer())
                plan[it.key()] = it.value().get<int>();
    } else {
        for (const auto& step : s.oracle) ++plan[step.tool];
    }
    return plan;
}

// Per-tool absolute deviation: every missing planned tool, every excess call
// of a planned tool, and every call to an unplanned tool costs one unit.
// Doing nothing is not a perfect plan — it is a fully missed one.
int tool_deviation(const std::map<std::string, int>& plan,
                   const std::map<std::string, int>& actual) {
    int deviation = 0;
    for (const auto& [tool, want] : plan) {
        const auto it = actual.find(tool);
        deviation += std::abs(want - (it == actual.end() ? 0 : it->second));
    }
    for (const auto& [tool, got] : actual)
        if (!plan.count(tool)) deviation += got;
    return deviation;
}

double agentic_score(int deviation, const Kpi& k) {
    double score = 100.0;
    score -= 10.0 * deviation;
    score -= 20.0 * k.redundant;
    score -= 25.0 * k.tool_failures;
    score -= 25.0 * k.tool_denied;
    score -= 30.0 * k.retries;
    if (k.hard_stop) score -= 50.0;
    return score < 0.0 ? 0.0 : score;
}
} // namespace

Agentic compute_agentic(const EventStream& stream, const Kpi& k,
                        const Scenario& s) {
    Agentic a;
    std::map<std::string, int> plan = tool_plan(s);
    if (plan.empty()) return a;
    a.has_plan = true;
    for (const auto& p : plan) a.plan_tools += p.second;

    std::map<std::string, int> actual;
    for (const auto& c : stream.calls) ++actual[c.name];
    const int actual_total = static_cast<int>(stream.calls.size());

    a.plan_by_tool = plan;
    a.actual_by_tool = actual;
    a.plan_deviation = tool_deviation(plan, actual);
    a.plan_ratio = actual_total > 0
                       ? static_cast<double>(a.plan_tools) /
                             static_cast<double>(actual_total)
                       : 0.0;
    a.efficiency_pct = a.plan_ratio * 100.0;
    if (a.efficiency_pct > 100.0) a.efficiency_pct = 100.0;
    a.score = agentic_score(a.plan_deviation, k);
    return a;
}

} // namespace bench
