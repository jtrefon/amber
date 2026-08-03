
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
    bool success = false;    double bullseye = 0.0;
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

// Continuous scoring (0..100 per sub-score and total). Unlike the binary
// success flag, every component earns partial credit, so the score
// discriminates between weak and strong models. Sub-scores:
//   correctness = 0.6*bullseye + 0.4*artifact  (template scenarios)
//              or 0.7*bullseye + 0.3*checks     (otherwise)
//   efficiency = 100 * clamp(1 - 0.10*excess_steps - 0.10*wasted
//                            - 0.20*redundant, 0, 1)       (non-template)
//              = 100 * clamp(1 - 0.10*excess_steps - 0.20*redundant, 0, 1)
//                                                          (template)
//   robustness = 100 * clamp(1 - 0.30*retries - 0.50*recoveries
//                            - 1.00*hard_stop, 0, 1)
//   adherence  = 100 * clamp(prompt_adherence - 0.25*forbidden_calls, 0, 1)
//   total      = 0.50*correctness + 0.20*efficiency + 0.15*robustness
//              + 0.15*adherence
struct Score {
    double correctness = 0.0;
    double efficiency = 0.0;
    double robustness = 0.0;
    double adherence = 0.0;
    double total = 0.0;
};

Score compute_score(const Kpi& k, const Scenario& s, double checks_ratio,
                    int forbidden_calls);

} // namespace bench

#endif // BENCH_KPI_H
