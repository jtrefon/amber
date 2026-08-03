
#include "bench/kpi.h"

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

} // namespace bench
