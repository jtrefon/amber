
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

std::string current_platform() noexcept {
    return is_darwin() ? "darwin" : "linux";
}

std::string lowercase(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_checks(const agent::json& j, Checks& out) {
    if (j.is_null()) return true;
    if (!j.is_object()) return false;
    if (j.contains("must_contain") && j["must_contain"].is_array())
        for (const auto& e : j["must_contain"])
            if (e.is_string()) out.must_contain.push_back(e.get<std::string>());
    if (j.contains("must_contain_any") && j["must_contain_any"].is_array()) {
        for (const auto& g : j["must_contain_any"]) {
            std::vector<std::string> group;
            if (g.is_array())
                for (const auto& e : g)
                    if (e.is_string()) group.push_back(e.get<std::string>());
            if (!group.empty()) out.must_contain_any.push_back(std::move(group));
        }
    }
    if (j.contains("must_not_contain") && j["must_not_contain"].is_array())
        for (const auto& e : j["must_not_contain"])
            if (e.is_string()) out.must_not_contain.push_back(e.get<std::string>());
    return true;
}

bool parse_step(const agent::json& j, ScenarioStep& out) {
    if (!j.is_object() || !j.contains("tool") || !j["tool"].is_string())
        return false;
    out.tool = j["tool"].get<std::string>();
    if (j.contains("args")) out.args = j["args"];
    if (j.contains("args_subset") && j["args_subset"].is_boolean())
        out.args_subset = j["args_subset"].get<bool>();
    if (j.contains("unordered") && j["unordered"].is_boolean())
        out.unordered = j["unordered"].get<bool>();
    return true;
}

} // namespace

std::optional<Scenario> load_scenario(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open scenario file: " + path;
        return std::nullopt;
    }
    agent::json j;
    try {
        j = agent::json::parse(f);
    } catch (const std::exception& e) {
        err = std::string("scenario is not valid JSON: ") + e.what();
        return std::nullopt;
    }

    Scenario s;
    if (!j.contains("name") || !j["name"].is_string()) {
        err = "scenario missing string field: name";
        return std::nullopt;
    }
    s.name = j["name"].get<std::string>();
    if (!j.contains("suite") || !j["suite"].is_string()) {
        err = "scenario missing string field: suite";
        return std::nullopt;
    }
    s.suite = j["suite"].get<std::string>();
    if (!j.contains("prompt") || !j["prompt"].is_string()) {
        err = "scenario missing string field: prompt";
        return std::nullopt;
    }
    s.prompt = j["prompt"].get<std::string>();

    if (j.contains("description") && j["description"].is_string())
        s.description = j["description"].get<std::string>();
    if (j.contains("platforms") && j["platforms"].is_array())
        for (const auto& p : j["platforms"])
            if (p.is_string()) s.platforms.push_back(p.get<std::string>());
    if (j.contains("hermetic_only") && j["hermetic_only"].is_boolean())
        s.hermetic_only = j["hermetic_only"].get<bool>();
    if (j.contains("model_profiles") && j["model_profiles"].is_array())
        for (const auto& p : j["model_profiles"])
            if (p.is_string()) s.model_profiles.push_back(p.get<std::string>());
    if (j.contains("setup") && j["setup"].is_object()) s.setup = j["setup"];
    if (j.contains("fake_replies") && j["fake_replies"].is_array())
        s.fake_replies = j["fake_replies"];
    if (j.contains("subagent_replies") && j["subagent_replies"].is_array())
        for (const auto& e : j["subagent_replies"])
            if (e.is_array()) s.subagent_replies.push_back(e);
    if (j.contains("stream") && j["stream"].is_boolean())
        s.stream = j["stream"].get<bool>();
    if (j.contains("task_tool") && j["task_tool"].is_boolean())
        s.task_tool = j["task_tool"].get<bool>();

    if (j.contains("oracle") && j["oracle"].is_array()) {
        for (const auto& e : j["oracle"]) {
            ScenarioStep step;
            if (!parse_step(e, step)) {
                err = "oracle step must be an object with a string 'tool'";
                return std::nullopt;
            }
            s.oracle.push_back(std::move(step));
        }
    }
    if (j.contains("checks_weight") && j["checks_weight"].is_number())
        s.checks_weight =
            std::max(0.0, std::min(1.0, j["checks_weight"].get<double>()));
    if (j.contains("forbidden_tools") && j["forbidden_tools"].is_array())
        for (const auto& t : j["forbidden_tools"])
            if (t.is_string()) s.forbidden_tools.push_back(t.get<std::string>());
    if (!parse_checks(j.value("prompt_checks", agent::json()), s.prompt_checks) ||
        !parse_checks(j.value("checks", agent::json()), s.checks)) {
        err = "checks must be objects with string arrays";
        return std::nullopt;
    }
    if (j.contains("template") && j["template"].is_string())
        s.template_dir = j["template"].get<std::string>();
    if (j.contains("optimal_plan") && j["optimal_plan"].is_object())
        s.optimal_plan = j["optimal_plan"];
    if (j.contains("difficulty") && j["difficulty"].is_number_integer())
        s.difficulty = std::max(1, std::min(5, j["difficulty"].get<int>()));
    if (j.contains("expected_steps") && j["expected_steps"].is_number_integer())
        s.expected_steps = j["expected_steps"].get<int>();
    if (j.contains("budget") && j["budget"].is_object()) {
        if (j["budget"].contains("max_steps") && j["budget"]["max_steps"].is_number_integer())
            s.max_steps = j["budget"]["max_steps"].get<int>();
        if (j["budget"].contains("max_wall_ms") && j["budget"]["max_wall_ms"].is_number_integer())
            s.max_wall_ms = j["budget"]["max_wall_ms"].get<long>();
    }
    return s;
}

bool platform_supported(const Scenario& s) noexcept {
    if (s.platforms.empty()) return true;
    const std::string host = current_platform();
    return std::any_of(s.platforms.begin(), s.platforms.end(),
                       [&host](const std::string& p) { return p == host; });
}

bool checks_pass(const Checks& c, const std::string& text) noexcept {
    const std::string folded = lowercase(text);
    const auto any_group = [&folded](const std::vector<std::string>& group) {
        return std::any_of(group.begin(), group.end(),
                           [&folded](const std::string& alt) {
                               return folded.find(lowercase(alt)) !=
                                      std::string::npos;
                           });
    };
    return std::all_of(c.must_contain.begin(), c.must_contain.end(),
                       [&folded](const std::string& need) {
                           return folded.find(lowercase(need)) !=
                                  std::string::npos;
                       }) &&
           std::all_of(c.must_contain_any.begin(), c.must_contain_any.end(),
                       any_group) &&
           std::all_of(c.must_not_contain.begin(), c.must_not_contain.end(),
                       [&folded](const std::string& banned) {
                           return folded.find(lowercase(banned)) ==
                                  std::string::npos;
                       });
}

double adherence(const Checks& c, const std::string& text) noexcept {
    const std::string folded = lowercase(text);
    size_t total = c.must_contain.size() + c.must_contain_any.size() +
                   c.must_not_contain.size();
    if (total == 0) return 1.0;
    size_t passed = 0;
    for (const auto& need : c.must_contain)
        if (folded.find(lowercase(need)) != std::string::npos) ++passed;
    for (const auto& group : c.must_contain_any)
        for (const auto& alt : group)
            if (folded.find(lowercase(alt)) != std::string::npos) {
                ++passed;
                break;
            }
    for (const auto& banned : c.must_not_contain)
        if (folded.find(lowercase(banned)) == std::string::npos) ++passed;
    return static_cast<double>(passed) / static_cast<double>(total);
}

} // namespace bench
