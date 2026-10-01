
#include "bench/scenario.h"

#include <algorithm>
#include <fstream>

namespace bench {

namespace {

bool is_darwin() noexcept {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

std::string current_platform() {
    return is_darwin() ? "darwin" : "linux";
}

std::string lowercase(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_step(const agent::json& j, ScenarioStep& out) {
    if (!j.is_object() || !j.contains("tool") || !j["tool"].is_string())
        return false;
    out.tool = j["tool"].get<std::string>();
    if (j.contains("args"))
        out.args = j["args"];
    if (j.contains("args_subset") && j["args_subset"].is_boolean())
        out.args_subset = j["args_subset"].get<bool>();
    if (j.contains("unordered") && j["unordered"].is_boolean())
        out.unordered = j["unordered"].get<bool>();
    return true;
}

// Typed field presence: a field counts only when the key exists and holds the
// expected type, which is what every optional scenario field requires.
bool has_string(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_string();
}
bool has_bool(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_boolean();
}
bool has_int(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_number_integer();
}
bool has_number(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_number();
}
bool has_object(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_object();
}
bool has_array(const agent::json& j, const char* key) {
    return j.contains(key) && j[key].is_array();
}

// Optional scalar fields: assign when present, leave the default otherwise.
void take_string(const agent::json& j, const char* key, std::string& out) {
    if (has_string(j, key))
        out = j[key].get<std::string>();
}
void take_bool(const agent::json& j, const char* key, bool& out) {
    if (has_bool(j, key))
        out = j[key].get<bool>();
}
void take_int(const agent::json& j, const char* key, int& out) {
    if (has_int(j, key))
        out = j[key].get<int>();
}
void take_long(const agent::json& j, const char* key, long& out) {
    if (has_int(j, key))
        out = j[key].get<long>();
}
void take_clamped_int(const agent::json& j, const char* key, int& out, int lo, int hi) {
    if (has_int(j, key))
        out = std::max(lo, std::min(hi, j[key].get<int>()));
}
void take_clamped_double(const agent::json& j, const char* key, double& out, double lo, double hi) {
    if (has_number(j, key))
        out = std::max(lo, std::min(hi, j[key].get<double>()));
}

// Optional composite fields: assign when present, leave the default otherwise.
void take_object(const agent::json& j, const char* key, agent::json& out) {
    if (has_object(j, key))
        out = j[key];
}
void take_array(const agent::json& j, const char* key, agent::json& out) {
    if (has_array(j, key))
        out = j[key];
}
void take_string_array(const agent::json& j, const char* key, std::vector<std::string>& out) {
    if (!has_array(j, key))
        return;
    for (const auto& e : j[key])
        if (e.is_string())
            out.push_back(e.get<std::string>());
}
void take_json_array(const agent::json& j, const char* key, std::vector<agent::json>& out) {
    if (!has_array(j, key))
        return;
    for (const auto& e : j[key])
        if (e.is_array())
            out.push_back(e);
}

// Any-of groups: each inner array contributes one group, empty strings and
// non-array members are dropped, and an all-empty group is not a group.
void take_string_groups(const agent::json& j, const char* key,
                        std::vector<std::vector<std::string>>& out) {
    if (!has_array(j, key))
        return;
    for (const auto& g : j[key]) {
        std::vector<std::string> group;
        if (g.is_array())
            for (const auto& e : g) {
                if (!e.is_string())
                    continue;
                const std::string alt = e.get<std::string>();
                if (!alt.empty())
                    group.push_back(alt);
            }
        if (!group.empty())
            out.push_back(std::move(group));
    }
}

// Checks are either absent (null) or an object; anything else is malformed.
bool parse_checks(const agent::json& j, Checks& out) {
    if (j.is_null())
        return true;
    if (!j.is_object())
        return false;
    take_string_array(j, "must_contain", out.must_contain);
    take_string_groups(j, "must_contain_any", out.must_contain_any);
    take_string_array(j, "must_not_contain", out.must_not_contain);
    return true;
}

// Required string field: the only fields whose absence is fatal.
bool require_string(const agent::json& j, const char* key, std::string& out, std::string& err) {
    if (!has_string(j, key)) {
        err = std::string("scenario missing string field: ") + key;
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

bool read_scenario_json(const std::string& path, agent::json& j, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open scenario file: " + path;
        return false;
    }
    try {
        j = agent::json::parse(f);
    } catch (const std::exception& e) {
        err = std::string("scenario is not valid JSON: ") + e.what();
        return false;
    }
    return true;
}

bool take_oracle(const agent::json& j, std::vector<ScenarioStep>& out, std::string& err) {
    if (!has_array(j, "oracle"))
        return true;
    for (const auto& e : j["oracle"]) {
        ScenarioStep step;
        if (!parse_step(e, step)) {
            err = "oracle step must be an object with a string 'tool'";
            return false;
        }
        out.push_back(std::move(step));
    }
    return true;
}

bool take_checks(const agent::json& j, Scenario& s, std::string& err) {
    if (!parse_checks(j.value("prompt_checks", agent::json()), s.prompt_checks) ||
        !parse_checks(j.value("checks", agent::json()), s.checks)) {
        err = "checks must be objects with string arrays";
        return false;
    }
    return true;
}

void take_budget(const agent::json& j, Scenario& s) {
    if (!has_object(j, "budget"))
        return;
    const agent::json& b = j["budget"];
    take_int(b, "max_steps", s.max_steps);
    take_long(b, "max_wall_ms", s.max_wall_ms);
}

} // namespace

std::optional<Scenario> load_scenario(const std::string& path, std::string& err) {
    agent::json j;
    if (!read_scenario_json(path, j, err))
        return std::nullopt;

    Scenario s;
    if (!require_string(j, "name", s.name, err) || !require_string(j, "suite", s.suite, err) ||
        !require_string(j, "prompt", s.prompt, err))
        return std::nullopt;

    take_string(j, "description", s.description);
    take_string_array(j, "platforms", s.platforms);
    take_bool(j, "hermetic_only", s.hermetic_only);
    take_string_array(j, "model_profiles", s.model_profiles);
    take_object(j, "setup", s.setup);
    take_array(j, "fake_replies", s.fake_replies);
    take_json_array(j, "subagent_replies", s.subagent_replies);
    take_bool(j, "stream", s.stream);
    take_bool(j, "detection_loop", s.detection_loop);
    take_bool(j, "detection_duplicate", s.detection_duplicate);
    if (!take_oracle(j, s.oracle, err))
        return std::nullopt;
    take_clamped_double(j, "checks_weight", s.checks_weight, 0.0, 1.0);
    take_string_array(j, "forbidden_tools", s.forbidden_tools);
    if (!take_checks(j, s, err))
        return std::nullopt;
    take_string(j, "template", s.template_dir);
    take_object(j, "optimal_plan", s.optimal_plan);
    take_clamped_int(j, "difficulty", s.difficulty, 1, 6);
    take_int(j, "expected_steps", s.expected_steps);
    take_budget(j, s);
    return s;
}

bool platform_supported(const Scenario& s) {
    if (s.platforms.empty())
        return true;
    const std::string host = current_platform();
    return std::any_of(s.platforms.begin(), s.platforms.end(),
                       [&host](const std::string& p) { return p == host; });
}

bool checks_pass(const Checks& c, const std::string& text) noexcept {
    const std::string folded = lowercase(text);
    const auto any_group = [&folded](const std::vector<std::string>& group) {
        return std::any_of(group.begin(), group.end(), [&folded](const std::string& alt) {
            return folded.find(lowercase(alt)) != std::string::npos;
        });
    };
    return std::all_of(c.must_contain.begin(), c.must_contain.end(),
                       [&folded](const std::string& need) {
                           return folded.find(lowercase(need)) != std::string::npos;
                       }) &&
           std::all_of(c.must_contain_any.begin(), c.must_contain_any.end(), any_group) &&
           std::all_of(c.must_not_contain.begin(), c.must_not_contain.end(),
                       [&folded](const std::string& banned) {
                           return folded.find(lowercase(banned)) == std::string::npos;
                       });
}

double adherence(const Checks& c, const std::string& text) noexcept {
    const std::string folded = lowercase(text);
    size_t total = c.must_contain.size() + c.must_contain_any.size() + c.must_not_contain.size();
    if (total == 0)
        return 1.0;
    size_t passed = 0;
    for (const auto& need : c.must_contain)
        if (folded.find(lowercase(need)) != std::string::npos)
            ++passed;
    for (const auto& group : c.must_contain_any)
        for (const auto& alt : group)
            if (folded.find(lowercase(alt)) != std::string::npos) {
                ++passed;
                break;
            }
    for (const auto& banned : c.must_not_contain)
        if (folded.find(lowercase(banned)) == std::string::npos)
            ++passed;
    return static_cast<double>(passed) / static_cast<double>(total);
}

} // namespace bench
