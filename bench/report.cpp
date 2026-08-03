
#include "bench/report.h"

#include <sstream>

#include "agent/tool.h"

namespace bench {

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta) {
    std::ostringstream out;
    out << "amber-bench run " << meta.run_id << " [" << meta.mode << ", "
        << (meta.profile.empty() ? "default" : meta.profile) << ", "
        << meta.model << "]\n";
    out << "engine " << meta.engine_version << " @ " << meta.timestamp << "\n";
    int passed = 0;
    for (const auto& r : reports) {
        const char* verdict = r.kpi.success ? "PASS" : "FAIL";
        if (r.kpi.success) ++passed;
        out << verdict << "  " << r.name << " (" << r.suite << ")"
            << "  bullseye=" << r.kpi.bullseye
            << " steps=" << r.kpi.steps
            << " wasted=" << r.kpi.wasted
            << " wall=" << r.kpi.wall_ms << "ms"
            << " retries=" << r.kpi.retries
            << " recoveries=" << r.kpi.recoveries
            << "\n";
        if (!r.failures.empty()) {
            for (const auto& f : r.failures) out << "    - " << f << "\n";
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
    agent::json arr = agent::json::array();
    for (const auto& r : reports) {
        agent::json j = agent::json::object();
        j["name"] = r.name;
        j["suite"] = r.suite;
        j["success"] = r.kpi.success;
        j["bullseye"] = r.kpi.bullseye;
        j["tool_call_accuracy"] = r.kpi.tool_call_accuracy;
        j["arg_precision"] = r.kpi.arg_precision;
        j["steps"] = r.kpi.steps;
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
        j["failures"] = r.failures;
        arr.push_back(std::move(j));
    }
    out["scenarios"] = std::move(arr);
    return out.dump(2) + "\n";
}

} // namespace bench
