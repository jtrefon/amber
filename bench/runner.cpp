
// RED: stub — runner not implemented yet (execution error).

#include "bench/runner.h"

namespace bench {

ScenarioReport run_one_scenario(const Scenario& s, const RunOptions&,
                                const RunMeta&, std::string& err) {
    err = "not implemented";
    return ScenarioReport{s.name, s.suite, Kpi{}, {"runner not implemented"}};
}

std::vector<ScenarioReport> run_scenarios(const std::vector<Scenario>&,
                                          const RunOptions&, const RunMeta&) {
    return {};
}

} // namespace bench
