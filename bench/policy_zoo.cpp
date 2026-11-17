#include "bench/policy_zoo.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <utility>

#include "bench/scenario.h"

namespace bench {

namespace {

PolicyReply final_reply(std::string text) {
    PolicyReply reply;
    reply.content = std::move(text);
    return reply;
}

PolicyReply call_reply(const std::string& tool, const agent::json& args, int step) {
    PolicyReply reply;
    agent::json function = {{"name", tool}, {"arguments", args.dump()}};
    agent::json call = {
        {"id", "zoo-" + std::to_string(step)}, {"type", "function"}, {"function", function}};
    reply.tool_calls = agent::json::array();
    reply.tool_calls.push_back(std::move(call));
    return reply;
}

std::string first_file(const Scenario* scenario) {
    if (scenario && scenario->setup.is_object() && scenario->setup.contains("files") &&
        scenario->setup["files"].is_object() && !scenario->setup["files"].empty()) {
        return scenario->setup["files"].begin().key();
    }
    return "a.txt";
}

class LazyPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext&) override { return final_reply("done"); }
};

class HallucinatorPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext&) override { return final_reply("a.txt contains hello."); }
};

class QuitterPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext& context) override {
        if (context.step == 0)
            return call_reply("read", {{"path", "missing.txt"}}, context.step);
        return final_reply("I can't finish.");
    }
};

class OverEagerPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext& context) override {
        if (context.step == 0) {
            const std::string path = first_file(context.scenario);
            return call_reply(
                "write",
                {{"path", path},
                 {"edits", agent::json::array({{{"old", "hello"}, {"new", "done"}}})}},
                context.step);
        }
        return final_reply("done");
    }
};

class WrongToolPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext& context) override {
        const std::string path = first_file(context.scenario);
        if (context.step == 0)
            return call_reply("bash", {{"command", "cat " + path}}, context.step);
        if (context.step == 1)
            return call_reply("read", {{"path", path}}, context.step);
        return final_reply("done");
    }
};

class RetrierPolicy : public Policy {
public:
    PolicyReply next(const PolicyContext& context) override {
        const std::string path = first_file(context.scenario);
        if (context.step == 0)
            return call_reply("read", {{"path", "missing.txt"}}, context.step);
        if (context.step == 1)
            return call_reply("read", {{"path", path}}, context.step);
        return final_reply("done");
    }
};

std::string join_failures(const std::vector<std::string>& failures) {
    std::string out;
    for (const std::string& failure : failures) {
        if (!out.empty())
            out += " | ";
        out += failure;
    }
    return out;
}

bool check_success(const agent::json& expected, const PolicyMatrixRow& row, std::string& err) {
    if (!expected.contains("success"))
        return true;
    if (expected["success"].get<bool>() == row.success)
        return true;
    err = "success expected ";
    err += expected["success"].get<bool>() ? "true" : "false";
    return false;
}

bool check_score(const agent::json& expected, const PolicyMatrixRow& row, std::string& err) {
    if (expected.contains("min_score") && row.score < expected["min_score"].get<double>()) {
        err = "score below min_score";
        return false;
    }
    if (expected.contains("max_score") && row.score > expected["max_score"].get<double>()) {
        err = "score above max_score";
        return false;
    }
    return true;
}

bool check_count(const agent::json& expected, const char* key, int actual, const char* label,
                 std::string& err) {
    if (!expected.contains(key) || actual >= expected[key].get<int>())
        return true;
    err = label;
    return false;
}

bool check_reason(const agent::json& expected, const PolicyMatrixRow& row, std::string& err) {
    if (!expected.contains("reason_contains"))
        return true;
    const agent::json& wanted = expected["reason_contains"];
    const std::string text = join_failures(row.failures);
    if (wanted.is_string()) {
        if (text.find(wanted.get<std::string>()) != std::string::npos)
            return true;
        err = "failure reason missing: " + wanted.get<std::string>();
        return false;
    }
    if (wanted.is_array()) {
        for (const auto& item : wanted) {
            if (item.is_string() && text.find(item.get<std::string>()) != std::string::npos)
                return true;
        }
        err = "none of the required failure reasons matched";
        return false;
    }
    return true;
}

bool expectation_matches(const agent::json& expected, const PolicyMatrixRow& row,
                         std::string& err) {
    if (!check_success(expected, row, err))
        return false;
    if (!check_score(expected, row, err))
        return false;
    if (!check_count(expected, "min_recoveries", row.recoveries, "recoveries below min_recoveries",
                     err))
        return false;
    if (!check_count(expected, "min_tool_failures", row.tool_failures,
                     "tool failures below min_tool_failures", err))
        return false;
    return check_reason(expected, row, err);
}

} // namespace

std::vector<std::string> policy_zoo_names() {
    return {"planner", "lazy", "wrong_tool", "hallucinator", "quitter", "over_eager", "retrier"};
}

bool is_policy_zoo_name(const std::string& name) {
    const std::vector<std::string> names = policy_zoo_names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::unique_ptr<Policy> make_policy_zoo_member(const std::string& name, const Scenario& scenario,
                                               std::string& err) {
    if (name == "planner") {
        if (scenario.policy.empty()) {
            err = "the planner policy needs a scenario 'policy' block";
            return nullptr;
        }
        return make_state_machine_policy(scenario.policy, err);
    }
    if (name == "lazy")
        return std::make_unique<LazyPolicy>();
    if (name == "hallucinator")
        return std::make_unique<HallucinatorPolicy>();
    if (name == "quitter")
        return std::make_unique<QuitterPolicy>();
    if (name == "over_eager")
        return std::make_unique<OverEagerPolicy>();
    if (name == "wrong_tool")
        return std::make_unique<WrongToolPolicy>();
    if (name == "retrier")
        return std::make_unique<RetrierPolicy>();
    err = "unknown policy: " + name;
    return nullptr;
}

bool check_policy_expectations(const std::string& path, const std::vector<PolicyMatrixRow>& rows,
                               std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot read policy expectations: " + path;
        return false;
    }
    agent::json document = agent::json::parse(in, nullptr, false);
    if (document.is_discarded() || !document.is_object() || !document.contains("scenarios") ||
        !document["scenarios"].is_object()) {
        err = "invalid policy expectations: " + path;
        return false;
    }
    for (const PolicyMatrixRow& row : rows) {
        const agent::json& scenarios = document["scenarios"];
        if (!scenarios.contains(row.scenario)) {
            err = "no expectations for scenario: " + row.scenario;
            return false;
        }
        const agent::json& policies = scenarios[row.scenario];
        if (!policies.is_object() || !policies.contains("policies") ||
            !policies["policies"].is_object() || !policies["policies"].contains(row.policy)) {
            err = "no expectations for " + row.scenario + "/" + row.policy;
            return false;
        }
        std::string detail;
        if (!expectation_matches(policies["policies"][row.policy], row, detail)) {
            err = row.scenario;
            err += "/";
            err += row.policy;
            err += ": ";
            err += detail;
            return false;
        }
    }
    return true;
}

} // namespace bench
