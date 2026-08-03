
// RED: stub — loader not implemented yet (fails every scenario).

#include "bench/scenario.h"

namespace bench {

std::optional<Scenario> load_scenario(const std::string&, std::string& err) {
    err = "not implemented";
    return std::nullopt;
}

bool platform_supported(const Scenario&) noexcept { return false; }

bool checks_pass(const Checks&, const std::string&) noexcept { return false; }

double adherence(const Checks&, const std::string&) noexcept { return 0.0; }

} // namespace bench
