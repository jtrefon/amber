
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
            ++p;
            ++t;
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
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

namespace {
// The basename of a path (after the last '/'); the input when there is none.
std::string basename(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// An expectation containing '*' is a glob.
bool is_glob(const agent::json& expected) {
    return expected.is_string() && expected.get<std::string>().find('*') != std::string::npos;
}

bool is_bare_name(const std::string& s) {
    return s.find('/') == std::string::npos;
}

// Nested relative expectation: "src/header.h" matches any absolute path ending
// in exactly "/src/header.h" (never a different directory with the same leaf).
bool nested_relative_matches(const std::string& e, const std::string& a) {
    return a[0] == '/' && a.size() > e.size() + 1 &&
           a.compare(a.size() - e.size(), e.size(), e) == 0 && a[a.size() - e.size() - 1] == '/';
}

// Path normalization: live agents read workspace files via their absolute
// paths (the tools resolve them); an oracle expecting a bare relative name
// must still match — compare basenames when exactly one side is a bare
// filename (nested expectations stay exact).
bool path_matches(const std::string& e, const std::string& a) {
    const bool e_bare = is_bare_name(e);
    const bool a_bare = is_bare_name(a);
    if (e_bare != a_bare)
        return !e.empty() && !a.empty() && basename(e) == basename(a);
    if (!e_bare && !a_bare)
        return nested_relative_matches(e, a);
    return false;
}
} // namespace

bool value_matches(const agent::json& expected, const agent::json& actual) {
    if (is_glob(expected))
        return actual.is_string() &&
               glob_match(expected.get<std::string>(), actual.get<std::string>());
    if (expected == actual)
        return true;
    if (expected.is_string() && actual.is_string())
        return path_matches(expected.get<std::string>(), actual.get<std::string>());
    return false;
}

// Keys of an args object (empty when args is null / not an object).
std::vector<std::string> keys(const agent::json& j) {
    std::vector<std::string> out;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            out.push_back(it.key());
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
    if (step.tool != call.name)
        return -1.0;
    if (step.args.is_null())
        return 1.0;
    const std::vector<std::string> expected = keys(step.args);
    if (expected.empty())
        return 1.0;
    if (!call.args.is_object())
        return -1.0;
    if (!step.args_subset) {
        const std::vector<std::string> actual = keys(call.args);
        if (actual.size() != expected.size())
            return -1.0;
        for (const auto& k : expected)
            if (!call.args.contains(k))
                return -1.0;
    }
    for (const auto& k : expected) {
        if (!call.args.contains(k))
            return -1.0;
        if (!value_matches(step.args[k], call.args[k]))
            return -1.0;
    }
    size_t matched = 0;
    for (const auto& k : expected) {
        if (value_matches(step.args[k], call.args[k]))
            ++matched;
    }
    return static_cast<double>(matched) / static_cast<double>(expected.size());
}

std::string canonical_args(const agent::json& j) {
    return j.is_string() ? j.get<std::string>() : j.dump();
}

// Skip past steps that are already matched or that may match out of order.
void advance_ordered(const std::vector<ScenarioStep>& oracle, const std::vector<StepState>& steps,
                     size_t& next_ordered) {
    while (next_ordered < oracle.size() &&
           (oracle[next_ordered].unordered || steps[next_ordered].matched))
        ++next_ordered;
}

// Match the next in-order step. Returns true when the call consumed it.
bool match_next_ordered(const std::vector<ScenarioStep>& oracle, std::vector<StepState>& steps,
                        size_t& next_ordered, const ToolCallEvent& call, size_t ci,
                        OracleResult& r) {
    if (next_ordered >= oracle.size() || oracle[next_ordered].unordered)
        return false;
    const double p = match_step(oracle[next_ordered], call);
    if (p < 0.0)
        return false;
    steps[next_ordered] = {true, static_cast<int>(ci), p};
    ++r.on_oracle_calls;
    ++next_ordered;
    return true;
}

// Otherwise try the steps marked unordered. Returns true when one consumed it.
bool match_any_unordered(const std::vector<ScenarioStep>& oracle, std::vector<StepState>& steps,
                         const ToolCallEvent& call, size_t ci, OracleResult& r) {
    for (size_t si = 0; si < oracle.size(); ++si) {
        if (!oracle[si].unordered || steps[si].matched)
            continue;
        const double p = match_step(oracle[si], call);
        if (p < 0.0)
            continue;
        steps[si] = {true, static_cast<int>(ci), p};
        ++r.on_oracle_calls;
        return true;
    }
    return false;
}

// Repeat calls of the same tool+args beyond the first.
int count_redundant(const std::vector<ToolCallEvent>& calls) {
    std::map<std::string, int> seen;
    int redundant = 0;
    for (const auto& c : calls) {
        const std::string fp = c.name + "|" + canonical_args(c.args);
        redundant += (seen[fp]++ > 0) ? 1 : 0;
    }
    return redundant;
}

} // namespace

OracleResult score_oracle(const std::vector<ScenarioStep>& oracle,
                          const std::vector<ToolCallEvent>& calls) {
    OracleResult r;
    r.total_steps = static_cast<int>(oracle.size());
    r.total_calls = static_cast<int>(calls.size());
    if (r.total_steps == 0) {
        r.success = true;
        r.bullseye = 1.0;
        r.arg_precision = 1.0;
        return r;
    }

    std::vector<StepState> steps(oracle.size());
    size_t next_ordered = 0;
    double precision_sum = 0.0;

    for (size_t ci = 0; ci < calls.size(); ++ci) {
        const ToolCallEvent& call = calls[ci];
        advance_ordered(oracle, steps, next_ordered);
        const bool matched = match_next_ordered(oracle, steps, next_ordered, call, ci, r) ||
                             match_any_unordered(oracle, steps, call, ci, r);
        if (!matched)
            ++r.wasted;
    }

    for (const auto& st : steps) {
        if (st.matched) {
            ++r.matched_steps;
            r.matched_call_indexes.push_back(static_cast<size_t>(st.call_index));
        }
        precision_sum += st.precision;
    }
    r.bullseye = static_cast<double>(r.matched_steps) / static_cast<double>(r.total_steps);
    r.arg_precision =
        r.matched_steps > 0 ? precision_sum / static_cast<double>(r.matched_steps) : 0.0;
    r.success = r.matched_steps == r.total_steps;
    r.redundant = count_redundant(calls);
    return r;
}

} // namespace bench
