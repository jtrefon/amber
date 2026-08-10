
#include "bench/oracle.h"

#include <algorithm>
#include <map>

namespace bench {

namespace {

// Glob-lite: '*' matches any run of characters.
bool glob_match(const std::string& pattern, const std::string& text) noexcept {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) {
            ++p; ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

namespace {
// The basename of a path (after the last '/'); the input when there is none.
std::string basename(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}
} // namespace

bool value_matches(const agent::json& expected, const agent::json& actual) {
    if (expected.is_string() &&
        expected.get<std::string>().find('*') != std::string::npos) {
        return actual.is_string() &&
               glob_match(expected.get<std::string>(),
                          actual.get<std::string>());
    }
    if (expected == actual) return true;
    // Path normalization: live agents read workspace files via their
    // absolute paths (the tools resolve them); an oracle expecting a bare
    // relative name must still match — compare basenames when exactly one
    // side is a bare filename (nested expectations stay exact).
    if (expected.is_string() && actual.is_string()) {
        const std::string& e = expected.get<std::string>();
        const std::string& a = actual.get<std::string>();
        const bool e_bare = e.find('/') == std::string::npos;
        const bool a_bare = a.find('/') == std::string::npos;
        if (e_bare != a_bare && !e.empty() && !a.empty())
            return basename(e) == basename(a);
    }
    return false;
}

// Keys of an args object (empty when args is null / not an object).
std::vector<std::string> keys(const agent::json& j) {
    std::vector<std::string> out;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) out.push_back(it.key());
    }
    return out;
}

struct StepState {
    bool matched = false;
    int call_index = -1;
    double precision = 0.0;
};

// Score one call against one step. Returns precision (0..1) or -1 on miss.
double match_step(const ScenarioStep& step, const ToolCallEvent& call) {
    if (step.tool != call.name) return -1.0;
    if (step.args.is_null()) return 1.0;
    const std::vector<std::string> expected = keys(step.args);
    if (expected.empty()) return 1.0;
    if (!call.args.is_object()) return -1.0;
    if (!step.args_subset) {
        const std::vector<std::string> actual = keys(call.args);
        if (actual.size() != expected.size()) return -1.0;
        for (const auto& k : expected)
            if (!call.args.contains(k)) return -1.0;
    }
    for (const auto& k : expected) {
        if (!call.args.contains(k)) return -1.0;
        if (!value_matches(step.args[k], call.args[k])) return -1.0;
    }
    size_t matched = 0;
    for (const auto& k : expected) {
        if (value_matches(step.args[k], call.args[k])) ++matched;
    }
    return static_cast<double>(matched) / static_cast<double>(expected.size());
}

std::string canonical_args(const agent::json& j) {
    return j.is_string() ? j.get<std::string>() : j.dump();
}

} // namespace

OracleResult score_oracle(const std::vector<ScenarioStep>& oracle,
                          const std::vector<ToolCallEvent>& calls) {
    OracleResult r;
    r.total_steps = static_cast<int>(oracle.size());
    if (r.total_steps == 0) {
        r.success = true;
        r.bullseye = 1.0;
        r.arg_precision = 1.0;
        r.total_calls = static_cast<int>(calls.size());
        return r;
    }

    std::vector<StepState> steps(oracle.size());
    size_t next_ordered = 0;
    double precision_sum = 0.0;

    for (size_t ci = 0; ci < calls.size(); ++ci) {
        const ToolCallEvent& call = calls[ci];
        bool matched = false;

        while (next_ordered < oracle.size() &&
               (oracle[next_ordered].unordered || steps[next_ordered].matched))
            ++next_ordered;

        if (next_ordered < oracle.size() && !oracle[next_ordered].unordered) {
            double p = match_step(oracle[next_ordered], call);
            if (p >= 0.0) {
                steps[next_ordered] = {true, static_cast<int>(ci), p};
                ++r.on_oracle_calls;
                ++next_ordered;
                matched = true;
            }
        }
        if (!matched) {
            for (size_t si = 0; si < oracle.size(); ++si) {
                if (!oracle[si].unordered || steps[si].matched) continue;
                double p = match_step(oracle[si], call);
                if (p >= 0.0) {
                    steps[si] = {true, static_cast<int>(ci), p};
                    ++r.on_oracle_calls;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) ++r.wasted;
    }

    for (const auto& st : steps) {
        if (st.matched) {
            ++r.matched_steps;
            r.matched_call_indexes.push_back(static_cast<size_t>(st.call_index));
        }
        precision_sum += st.precision;
    }
    r.bullseye = static_cast<double>(r.matched_steps) /
                 static_cast<double>(r.total_steps);
    r.arg_precision = r.matched_steps > 0
                          ? precision_sum / static_cast<double>(r.matched_steps)
                          : 0.0;
    r.success = r.matched_steps == r.total_steps;
    r.total_calls = static_cast<int>(calls.size());

    std::map<std::string, int> seen;
    for (const auto& c : calls) {
        std::string fp = c.name + "|" + canonical_args(c.args);
        r.redundant += (seen[fp]++ > 0) ? 1 : 0;
    }
    return r;
}

} // namespace bench
