#include "bench/probe.h"

#include <map>
#include <utility>

namespace bench {

namespace {
struct Registry {
    std::vector<ProbeResult> pending;
    std::map<std::string, std::function<bool(ProbeResult&)>> runners;
};
Registry& registry() {
    static Registry r;
    return r;
}
} // namespace

void register_probe(ProbeResult result,
                    const std::function<bool(ProbeResult&)>& run) {
    registry().pending.push_back(std::move(result));
    registry().runners[result.name] = run;
}

std::vector<ProbeResult> run_all_probes() {
    std::vector<ProbeResult> out;
    for (const auto& def : registry().pending) {
        ProbeResult r = def;
        const auto it = registry().runners.find(r.name);
        if (it == registry().runners.end()) {
            r.passed = false;
            r.detail = "no runner registered";
        } else if (it->second(r)) {
            r.passed = true;
        }
        out.push_back(std::move(r));
    }
    return out;
}

double HarnessScorecard::family_integrity(const std::string& family) const
    noexcept {
    const auto it = families.find(family);
    if (it == families.end() || it->second.second == 0) return 0.0;
    return static_cast<double>(it->second.first) /
           static_cast<double>(it->second.second);
}

HarnessScorecard aggregate_probes(const std::vector<ProbeResult>& probes) {
    HarnessScorecard sc;
    sc.probes = probes;
    for (const auto& p : probes) {
        ++sc.total;
        if (p.passed) ++sc.passed;
        auto& f = sc.families[p.family];
        ++f.second;
        if (p.passed) ++f.first;
    }
    sc.integrity = sc.total > 0
                       ? static_cast<double>(sc.passed) / sc.total
                       : 0.0;
    return sc;
}

} // namespace bench