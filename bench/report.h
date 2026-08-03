
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
    std::string engine_version;
    std::string timestamp;
};

struct ScenarioReport {
    std::string name;
    std::string suite;
    Kpi kpi;
    std::string final_text;              // the agent's final answer
    std::vector<std::pair<std::string, std::string>> tool_calls;  // name, args
    bool templated = false;              // static-template scenario
    std::vector<std::string> failures;
};

std::string render_text(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

std::string render_json(const std::vector<ScenarioReport>& reports,
                        const RunMeta& meta);

} // namespace bench

#endif // BENCH_REPORT_H
