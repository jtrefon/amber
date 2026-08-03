
// RED: stub — oracle scoring not implemented yet (never matches).

#include "bench/oracle.h"

namespace bench {

OracleResult score_oracle(const std::vector<ScenarioStep>&,
                          const std::vector<ToolCallEvent>&) {
    return OracleResult{};
}

} // namespace bench
