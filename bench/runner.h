
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
    std::string profile; // profiles.json key ("" = none)
    int repeat = 1;
    std::string model;        // explicit model override (live)
    double temperature = -1;  // <0 = leave default
    std::string thinking;     // "" = leave default; "on"/"off"/"auto"
    int thinking_budget = -1; // <0 = leave default
    std::string debug_dir;    // per-scenario wire + conversation logs
    // Plugins this run switches off, applied without persisting. A run that
    // disables nothing measures the shipped configuration.
    std::vector<std::string> disable_plugins;
};

// Run one scenario to completion; returns a full report. `err` carries the
// failure description when the scenario could not be executed at all.
//
// `meta` comes back with `plugins` filled in the first time it is empty: the
// plugin set follows from the run options, so every scenario in a run has the
// same one, and recording it once is enough to make two runs comparable.
ScenarioReport run_one_scenario(const Scenario& s, const RunOptions& opts, RunMeta& meta,
                                std::string& err);

// Run a batch of scenarios serially (resource KPIs need isolation).
std::vector<ScenarioReport> run_scenarios(const std::vector<Scenario>& scenarios,
                                          const RunOptions& opts, RunMeta& meta);

} // namespace bench

#endif // BENCH_RUNNER_H
