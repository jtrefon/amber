
// RED: stub — report rendering not implemented yet (empty output).

#include "bench/report.h"

namespace bench {

std::string render_text(const std::vector<ScenarioReport>&, const RunMeta&) {
    return "";
}

std::string render_json(const std::vector<ScenarioReport>&, const RunMeta&) {
    return "{}";
}

} // namespace bench
