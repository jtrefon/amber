
// RED: stub — KPI computation not implemented yet (empty record).

#include "bench/kpi.h"

namespace bench {

Kpi compute_kpi(const EventStream&, const OracleResult&, const ResourceMeter&,
                const TemplateResult&, const Checks&, const std::string&,
                long, long) {
    return Kpi{};
}

bool kpi_success(const Kpi&, const Scenario&) noexcept { return false; }

} // namespace bench
