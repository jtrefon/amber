
#ifndef BENCH_KPI_H
#define BENCH_KPI_H

// KPI aggregation: fold the recorder's event stream, the oracle result, the
// resource sample and the template result into one scenario KPI record.

#include <string>

#include "bench/oracle.h"
#include "bench/recorder.h"
#include "bench/resources.h"
#include "bench/template.h"

namespace bench {

struct Kpi {
    bool success = false;
    double bullseye = 0.0;
    double tool_call_accuracy = 0.0;
    double arg_precision = 0.0;
    int steps = 0;
    int wasted = 0;
    int redundant = 0;
    int retries = 0;
    int recoveries = 0;
    int steers = 0;
    bool hard_stop = false;
    long wall_ms = 0;
    long bullseye_at_ms = 0;
    double ttft_ms = -1;
    double tps_avg = -1;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    long baseline_rss_kb = 0;
    long peak_rss_kb = 0;
    long cpu_ms = 0;
    int files_touched = 0;
    double artifact_score = 0.0;      // hidden tests passed / total (no template -> 1.0)
    bool compile_ok = false;          // no template -> true
    bool behavior_equivalent = false; // no template -> true
    double structure_checks = 0.0;    // no template -> 1.0
    double prompt_adherence = 0.0;
};

// Compute the KPI record. `final_text` is the run's final assistant reply.
// `bullseye_at_ms` is the wall time until the oracle was fully matched.
Kpi compute_kpi(const EventStream& stream, const OracleResult& oracle,
                const ResourceMeter& meter, const TemplateResult& tmpl,
                const Checks& prompt_checks, const std::string& final_text,
                long wall_ms, long bullseye_at_ms);

// Compose the success flag from the KPI record + budget enforcement.
bool kpi_success(const Kpi& k, const Scenario& s) noexcept;

} // namespace bench

#endif // BENCH_KPI_H
