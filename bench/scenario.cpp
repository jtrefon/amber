
#include "bench/scenario.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include "bench/policy.h"

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

bool take_policy(const agent::json& j, Scenario& s, std::string& err) {
    if (!j.contains("policy"))
        return true;
    if (!j["policy"].is_object()) {
        err = "'policy' must be an object";
        return false;
    }
    s.policy = j["policy"];
    return validate_state_machine(s.policy, err);
}

bool relative_path(const std::string& path) {
    if (path.empty())
        return false;
    const std::filesystem::path candidate(path);
    if (candidate.is_absolute())
        return false;
    return std::all_of(candidate.begin(), candidate.end(),
                       [](const std::filesystem::path& part) { return part != ".."; });
}

bool workspace_matcher(const agent::json& value, std::string& err) {
    if (value.is_string())
        return true;
    if (!value.is_object()) {
        err = "workspace expectation must be a string or an object";
        return false;
    }
    int matchers = 0;
    for (const char* key : {"exact", "contains", "regex"}) {
        if (!value.contains(key))
            continue;
        if (!value[key].is_string()) {
            err = std::string("workspace '") + key + "' must be a string";
            return false;
        }
        if (std::string(key) == "regex") {
            try {
                std::regex unused(value[key].get<std::string>());
                (void)unused;
            } catch (const std::regex_error&) {
                err = "workspace 'regex' is not a valid regular expression";
                return false;
            }
        }
        ++matchers;
    }
    if (matchers != 1) {
        err = "workspace expectation needs exactly one of exact/contains/regex";
        return false;
    }
    return true;
}

bool take_workspace_files(const agent::json& value, std::string& err) {
    if (!value.is_object()) {
        err = "'expected_workspace.files' must be an object";
        return false;
    }
    for (auto file = value.begin(); file != value.end(); ++file) {
        if (!relative_path(file.key())) {
            err = "workspace path is not relative: " + file.key();
            return false;
        }
        if (!workspace_matcher(file.value(), err))
            return false;
    }
    return true;
}

bool take_workspace_absent(const agent::json& value, std::string& err) {
    if (!value.is_array()) {
        err = "'expected_workspace.absent' must be an array";
        return false;
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](const agent::json& path) {
        return path.is_string() && relative_path(path.get<std::string>());
    });
    if (!valid)
        err = "'expected_workspace.absent' needs relative paths";
    return valid;
}

bool take_workspace_diff(const agent::json& value, std::string& err) {
    if (!value.is_object()) {
        err = "'expected_workspace.diff_contains' must be an object";
        return false;
    }
    for (auto file = value.begin(); file != value.end(); ++file) {
        if (!relative_path(file.key()) || !file.value().is_array()) {
            err = "'expected_workspace.diff_contains' needs a path and string array";
            return false;
        }
        const bool valid =
            std::all_of(file.value().begin(), file.value().end(),
                        [](const agent::json& needle) { return needle.is_string(); });
        if (!valid) {
            err = "'expected_workspace.diff_contains' values must be strings";
            return false;
        }
    }
    return true;
}

bool take_workspace(const agent::json& j, Scenario& s, std::string& err) {
    if (!j.contains("expected_workspace"))
        return true;
    const agent::json& workspace = j["expected_workspace"];
    if (!workspace.is_object()) {
        err = "'expected_workspace' must be an object";
        return false;
    }
    for (auto it = workspace.begin(); it != workspace.end(); ++it) {
        const std::string& key = it.key();
        bool ok = false;
        if (key == "files")
            ok = take_workspace_files(it.value(), err);
        else if (key == "absent")
            ok = take_workspace_absent(it.value(), err);
        else if (key == "diff_contains")
            ok = take_workspace_diff(it.value(), err);
        else
            err = "unknown expected_workspace field: " + key;
        if (!ok)
            return false;
    }
    s.expected_workspace = workspace;
    return true;
}

bool valid_contract_field(const std::string& key, const agent::json& value, std::string& err) {
    if (key == "system_prefix_stable" || key == "no_system_after_first") {
        if (!value.is_boolean()) {
            err = "request_contract '" + key + "' must be a boolean";
            return false;
        }
        return true;
    }
    if (key == "tools_include" || key == "tools_exclude") {
        if (!value.is_array()) {
            err = "request_contract '" + key + "' must be an array";
            return false;
        }
        const bool valid = std::all_of(value.begin(), value.end(),
                                       [](const agent::json& name) { return name.is_string(); });
        if (!valid)
            err = "request_contract '" + key + "' values must be strings";
        return valid;
    }
    if (key == "cache_breakpoints") {
        if (!value.is_number_integer() || value.get<int>() < 0) {
            err = "request_contract 'cache_breakpoints' must be a non-negative integer";
            return false;
        }
        return true;
    }
    err = "unknown request_contract field: " + key;
    return false;
}

bool take_request_contract(const agent::json& j, Scenario& s, std::string& err) {
    if (!j.contains("request_contract"))
        return true;
    const agent::json& contract = j["request_contract"];
    if (!contract.is_object()) {
        err = "'request_contract' must be an object";
        return false;
    }
    for (auto it = contract.begin(); it != contract.end(); ++it)
        if (!valid_contract_field(it.key(), it.value(), err))
            return false;
    s.request_contract = contract;
    return true;
}

bool valid_fault(const agent::json& fault, std::string& err) {
    if (!fault.is_object() || !fault.contains("at_request") ||
        !fault["at_request"].is_number_integer() || fault["at_request"].get<int>() < 1 ||
        !fault.contains("kind") || !fault["kind"].is_string()) {
        err = "each fault needs at_request >= 1 and a string kind";
        return false;
    }
    static const std::set<std::string> kinds = {"http_429",       "http_500",    "disconnect",
                                                "malformed_json", "empty_reply", "stream_drop"};
    const std::string kind = fault["kind"].get<std::string>();
    if (kinds.count(kind) == 0) {
        err = "unknown fault kind: " + kind;
        return false;
    }
    if (fault.contains("after_chunks") &&
        (!fault["after_chunks"].is_number_integer() || fault["after_chunks"].get<int>() < 1)) {
        err = "'after_chunks' must be a positive integer";
        return false;
    }
    return true;
}

bool take_faults(const agent::json& j, Scenario& s, std::string& err) {
    if (!j.contains("faults"))
        return true;
    if (!j["faults"].is_array()) {
        err = "'faults' must be an array";
        return false;
    }
    for (const auto& fault : j["faults"]) {
        if (!valid_fault(fault, err))
            return false;
        s.faults.push_back(fault);
    }
    return true;
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
    take_int(b, "min_compactions", s.min_compactions);
    take_clamped_double(b, "max_compaction_ratio", s.max_compaction_ratio, 0.0, 1.0);
}

// A multi-turn scenario appends each turn's script and oracle to the scenario's
// effective stream. The runner then replays the script in turn order and the
// existing scoring sees one continuous conversation.
bool take_turns(const agent::json& j, Scenario& s, std::string& err) {
    if (!j.contains("turns"))
        return true;
    if (!j["turns"].is_array() || j["turns"].empty()) {
        err = "'turns' must be a non-empty array";
        return false;
    }
    for (const auto& item : j["turns"]) {
        if (!item.is_object()) {
            err = "each turn must be an object";
            return false;
        }
        Turn turn;
        if (!has_string(item, "prompt")) {
            err = "each turn needs a string 'prompt'";
            return false;
        }
        turn.prompt = item["prompt"].get<std::string>();
        if (!has_array(item, "fake_replies") || item["fake_replies"].empty()) {
            err = "each turn needs a non-empty 'fake_replies' array";
            return false;
        }
        turn.fake_replies = item["fake_replies"];
        if (!take_oracle(item, turn.oracle, err))
            return false;
        if (!parse_checks(item.value("checks", agent::json()), turn.checks)) {
            err = "turn checks must be an object with string arrays";
            return false;
        }
        for (const auto& e : turn.fake_replies)
            s.fake_replies.push_back(e);
        for (const auto& step : turn.oracle)
            s.oracle.push_back(step);
        s.turns.push_back(std::move(turn));
    }
    return true;
}

// The axes the harness can actually vary at runtime. Keep this closed: an
// unknown axis must fail at load, not silently do nothing.
bool valid_variant_axis(const std::string& key) {
    return key == "stream" || key == "context_size" || key == "approval";
}

bool valid_variant_value(const std::string& key, const agent::json& value, std::string& err) {
    if (key == "stream") {
        if (!value.is_boolean()) {
            err = "variant 'stream' values must be booleans";
            return false;
        }
        return true;
    }
    if (key == "context_size") {
        if (!value.is_number_integer() || value.get<int>() <= 0) {
            err = "variant 'context_size' values must be positive integers";
            return false;
        }
        return true;
    }
    if (key == "approval") {
        if (!value.is_string() ||
            (value.get<std::string>() != "allow" && value.get<std::string>() != "deny")) {
            err = "variant 'approval' values must be 'allow' or 'deny'";
            return false;
        }
        return true;
    }
    err = "unknown variant axis: " + key;
    return false;
}

std::string variant_label(const std::string& key, const agent::json& value) {
    if (key == "stream" && value.is_boolean())
        return value.get<bool>() ? "on" : "off";
    if (value.is_string())
        return value.get<std::string>();
    return value.dump();
}

bool apply_variant(Scenario& s, const std::string& key, const agent::json& value,
                   std::string& err) {
    if (key == "stream") {
        s.stream = value.get<bool>();
        return true;
    }
    if (key == "context_size") {
        s.context_size = value.get<int>();
        return true;
    }
    if (key == "approval") {
        s.allow_tools = value.get<std::string>() == "allow";
        return true;
    }
    err = "unknown variant axis: " + key;
    return false;
}

bool take_variants(const agent::json& j, Scenario& s, std::string& err) {
    if (!has_object(j, "variants"))
        return true;
    for (auto it = j["variants"].begin(); it != j["variants"].end(); ++it) {
        const std::string key = it.key();
        if (!valid_variant_axis(key)) {
            err = "unknown variant axis: " + key;
            return false;
        }
        if (!it.value().is_array() || it.value().empty()) {
            err = "variant '" + key + "' must be a non-empty array";
            return false;
        }
        for (const auto& value : it.value()) {
            if (!valid_variant_value(key, value, err))
                return false;
        }
        s.variants[key] = it.value().get<std::vector<agent::json>>();
    }
    return true;
}

bool take_extensions(const agent::json& j, Scenario& s, std::string& err) {
    if (!take_turns(j, s, err) || !take_variants(j, s, err))
        return false;
    if (s.turns.empty() && s.prompt.empty()) {
        err = "scenario missing string field: prompt";
        return false;
    }
    return true;
}

} // namespace

std::optional<Scenario> load_scenario(const std::string& path, std::string& err) {
    agent::json j;
    if (!read_scenario_json(path, j, err))
        return std::nullopt;

    Scenario s;
    if (!require_string(j, "name", s.name, err) || !require_string(j, "suite", s.suite, err))
        return std::nullopt;

    take_string(j, "prompt", s.prompt);
    take_string(j, "description", s.description);
    take_string_array(j, "platforms", s.platforms);
    take_bool(j, "hermetic_only", s.hermetic_only);
    take_string_array(j, "model_profiles", s.model_profiles);
    take_object(j, "setup", s.setup);
    take_array(j, "fake_replies", s.fake_replies);
    take_json_array(j, "subagent_replies", s.subagent_replies);
    if (!take_policy(j, s, err))
        return std::nullopt;
    if (!take_workspace(j, s, err) || !take_request_contract(j, s, err) || !take_faults(j, s, err))
        return std::nullopt;
    take_bool(j, "stream", s.stream);
    std::string approval;
    take_string(j, "approval", approval);
    s.allow_tools = approval == "allow";
    take_bool(j, "detection_loop", s.detection_loop);
    take_bool(j, "detection_duplicate", s.detection_duplicate);
    if (!take_oracle(j, s.oracle, err))
        return std::nullopt;
    take_clamped_double(j, "checks_weight", s.checks_weight, 0.0, 1.0);
    take_string_array(j, "forbidden_tools", s.forbidden_tools);
    if (!take_checks(j, s, err))
        return std::nullopt;
    if (!take_extensions(j, s, err))
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

// One `key=value` cell, or an empty vector with `err` set.
std::vector<Scenario> select_variant(const Scenario& s, const std::string& variant,
                                     std::string& err) {
    const size_t eq = variant.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= variant.size()) {
        err = "--variant expects key=value";
        return {};
    }
    const std::string key = variant.substr(0, eq);
    std::string want = variant.substr(eq + 1);
    auto it = s.variants.find(key);
    if (it == s.variants.end()) {
        err = "unknown variant axis: " + key;
        return {};
    }
    if (key == "stream") {
        if (want == "true")
            want = "on";
        if (want == "false")
            want = "off";
    }
    for (const auto& value : it->second) {
        if (variant_label(key, value) != want)
            continue;
        Scenario cell = s;
        if (!apply_variant(cell, key, value, err))
            return {};
        cell.name.append("@").append(key).append("=").append(want);
        return {cell};
    }
    err = "variant not declared: " + variant;
    return {};
}

// The bounded cross-product of every declared axis.
std::vector<Scenario> matrix_cells(const Scenario& s, std::string& err) {
    std::vector<Scenario> cells{s};
    for (const auto& [key, values] : s.variants) {
        std::vector<Scenario> next;
        for (const auto& cell : cells) {
            for (const auto& value : values) {
                Scenario expanded = cell;
                if (!apply_variant(expanded, key, value, err))
                    return {};
                expanded.name.append("@").append(key).append("=").append(variant_label(key, value));
                next.push_back(std::move(expanded));
            }
        }
        cells = std::move(next);
        if (cells.size() > 8) {
            err = "variant matrix exceeds 8 cells";
            return {};
        }
    }
    return cells;
}

std::vector<Scenario> expand_variants(const Scenario& s, bool matrix, const std::string& variant,
                                      std::string& err) {
    if (s.variants.empty())
        return {s};
    if (!variant.empty())
        return select_variant(s, variant, err);
    if (!matrix)
        return {s};
    return matrix_cells(s, err);
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
