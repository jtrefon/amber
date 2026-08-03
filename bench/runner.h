
#ifndef BENCH_RUNNER_H
#define BENCH_RUNNER_H

// Scenario runner: per-scenario temp workspace, Agent construction (mirrors
// src/main.cpp), recorder + resource meter wiring, execution, scoring.
// Hermetic mode uses FakeClient; live mode uses the engine's default
// HttpLLMClient factory. A fresh Agent per scenario (isolation).

#include <string>
#include <vector>

#include "bench/kpi.h"
#include "bench/report.h"
#include "bench/scenario.h"

namespace bench {

struct RunOptions {
    bool live = false;
    std::string profile;         // profiles.json key ("" = none)
    int repeat = 1;
    std::string model;           // explicit model override (live)
    double temperature = -1;     // <0 = leave default
};

// Run one scenario to completion; returns a full report. `err` carries the
// failure description when the scenario could not be executed at all.
ScenarioReport run_one_scenario(const Scenario& s, const RunOptions& opts,
                                const RunMeta& meta, std::string& err);

// Run a batch of scenarios serially (resource KPIs need isolation).
std::vector<ScenarioReport> run_scenarios(const std::vector<Scenario>& scenarios,
                                          const RunOptions& opts,
                                          const RunMeta& meta);

} // namespace bench

#endif // BENCH_RUNNER_H
