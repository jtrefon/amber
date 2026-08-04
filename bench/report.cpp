
#include "bench/report.h"

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

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta) {
    std::ostringstream out;
    out << "amber-bench run " << meta.run_id << " [" << meta.mode << ", "
        << (meta.profile.empty() ? "default" : meta.profile) << ", "
        << meta.model << "]\n";
    out << "engine " << meta.engine_version << " @ " << meta.timestamp << "\n";
    out << "model score: " << (run_score(reports) * 10.0) << "/1000\n";
    int passed = 0;
    for (const auto& r : reports) {
        const char* verdict = r.kpi.success ? "PASS" : "FAIL";
        if (r.kpi.success) ++passed;
        out << verdict << "  " << r.name << " (" << r.suite << ")"
            << "  score=" << static_cast<int>(r.score.total)
            << " d" << r.difficulty
            << " bullseye=" << r.kpi.bullseye
            << " steps=" << r.kpi.steps
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
        j["score_correctness"] = r.score.correctness;
        j["score_efficiency"] = r.score.efficiency;
        j["score_robustness"] = r.score.robustness;
        j["score_adherence"] = r.score.adherence;
        j["bullseye"] = r.kpi.bullseye;
        j["tool_call_accuracy"] = r.kpi.tool_call_accuracy;
        j["arg_precision"] = r.kpi.arg_precision;
        j["steps"] = r.kpi.steps;
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
        j["failures"] = r.failures;
        arr.push_back(std::move(j));
    }
    out["scenarios"] = std::move(arr);
    out["model_score"] = run_score(reports) * 10.0;
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
    for (const auto& r : reports) {
        tot_tools += r.kpi.tool_calls;
        tot_fail += r.kpi.tool_failures;
        tot_denied += r.kpi.tool_denied;
        tot_red += r.kpi.redundant;
        tot_retr += r.kpi.retries;
        tot_wall += r.kpi.wall_ms;
    }
    const int n = static_cast<int>(reports.size());
    out << "| metric | total | per scenario |\n|---|---|---|\n";
    out << "| tool calls | " << tot_tools << " | "
        << (n ? (tot_tools / static_cast<double>(n)) : 0.0) << " |\n";
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
    return out.str();
}

std::string render_markdown_comparison(
    const std::vector<std::pair<RunMeta, std::vector<ScenarioReport>>>& runs) {
    if (runs.empty()) return "";
    std::ostringstream out;
    out << "# Benchmark: harness score by model\n\n";
    for (const auto& run : runs)
        out << "- **" << run.first.model << "**: "
            << static_cast<int>(run_score(run.second) * 10.0) << "/1000\n";

    out << "\n| model | score | tools | failures | denied | redundant | "
           "retries | steps | wall (s) |\n";
    out << "|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& run : runs) {
        int tools = 0, fail = 0, denied = 0, red = 0, retr = 0, steps = 0;
        long wall = 0;
        for (const auto& r : run.second) {
            tools += r.kpi.tool_calls;
            fail += r.kpi.tool_failures;
            denied += r.kpi.tool_denied;
            red += r.kpi.redundant;
            retr += r.kpi.retries;
            steps += r.kpi.steps;
            wall += r.kpi.wall_ms;
        }
        out << "| " << run.first.model << " | "
            << static_cast<int>(run_score(run.second) * 10.0) << " | " << tools
            << " | " << fail << " | " << denied << " | " << red << " | "
            << retr << " | " << steps << " | " << (wall / 1000.0) << " |\n";
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

} // namespace bench
