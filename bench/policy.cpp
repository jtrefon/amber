#include "bench/policy.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace bench {

namespace {

enum class StateKind { Call, Final, Fail };

struct State {
    StateKind kind = StateKind::Final;
    std::string text; // final content or failure message
    agent::json call = agent::json::object();
    std::string next;
    agent::json when = agent::json::object();
};

struct Spec {
    std::string initial;
    int max_transitions = 16;
    bool bounded = false;
    int retries = 0;
    std::string on_error_next;
    std::map<std::string, State> states;
};

bool is_string_or_object(const agent::json& value) {
    return value.is_string() || value.is_object();
}

bool parse_condition(const agent::json& when, std::string& err) {
    if (!when.is_object()) {
        err = "'when' must be an object";
        return false;
    }
    for (auto it = when.begin(); it != when.end(); ++it) {
        const std::string& key = it.key();
        if (key == "last_tool_status" || key == "last_tool_name" || key == "last_result_contains") {
            if (!it.value().is_string()) {
                err = "condition '" + key + "' must be a string";
                return false;
            }
        } else if (key == "step") {
            if (!it.value().is_number_integer() || it.value().get<long>() < 0) {
                err = "condition 'step' must be a non-negative integer";
                return false;
            }
        } else if (key == "always") {
            if (!it.value().is_boolean()) {
                err = "condition 'always' must be a boolean";
                return false;
            }
        } else {
            err = "unknown condition: " + key;
            return false;
        }
    }
    return true;
}

bool parse_call(const agent::json& call, State& state, std::string& err) {
    if (!call.is_object()) {
        err = "'call' must be an object";
        return false;
    }
    if (!call.contains("tool") || !call["tool"].is_string() ||
        call["tool"].get<std::string>().empty()) {
        err = "'call.tool' must be a non-empty string";
        return false;
    }
    if (call.contains("args") && !is_string_or_object(call["args"])) {
        err = "'call.args' must be an object or a JSON string";
        return false;
    }
    state.kind = StateKind::Call;
    state.call = call;
    return true;
}

bool parse_state_action(const agent::json& value, State& state, std::string& err) {
    int actions = 0;
    if (value.contains("call")) {
        if (!parse_call(value["call"], state, err))
            return false;
        ++actions;
    }
    if (value.contains("final")) {
        if (!value["final"].is_string()) {
            err = "'final' must be a string";
            return false;
        }
        state.kind = StateKind::Final;
        state.text = value["final"].get<std::string>();
        ++actions;
    }
    if (value.contains("fail")) {
        if (!value["fail"].is_string()) {
            err = "'fail' must be a string";
            return false;
        }
        state.kind = StateKind::Fail;
        state.text = value["fail"].get<std::string>();
        ++actions;
    }
    if (actions != 1) {
        err = "a state needs exactly one of 'call', 'final' or 'fail'";
        return false;
    }
    return true;
}

bool parse_state_next(const agent::json& value, State& state, std::string& err) {
    if (value.contains("next")) {
        if (!value["next"].is_string() || value["next"].get<std::string>().empty()) {
            err = "'next' must be a non-empty string";
            return false;
        }
        state.next = value["next"].get<std::string>();
    }
    if (state.kind == StateKind::Call && state.next.empty()) {
        err = "a call state needs 'next'";
        return false;
    }
    if (state.kind != StateKind::Call && !state.next.empty()) {
        err = "a terminal state cannot also have 'next'";
        return false;
    }
    return true;
}

bool parse_state(const agent::json& value, State& state, std::string& err) {
    if (!value.is_object()) {
        err = "each state must be an object";
        return false;
    }
    if (!parse_state_action(value, state, err))
        return false;
    if (!parse_state_next(value, state, err))
        return false;
    if (value.contains("when")) {
        if (!parse_condition(value["when"], err))
            return false;
        state.when = value["when"];
    }
    return true;
}

bool parse_policy_header(const agent::json& value, Spec& spec, std::string& err) {
    if (!value.is_object()) {
        err = "policy must be an object";
        return false;
    }
    if (!value.contains("initial") || !value["initial"].is_string() ||
        value["initial"].get<std::string>().empty()) {
        err = "policy needs a non-empty 'initial'";
        return false;
    }
    spec.initial = value["initial"].get<std::string>();
    if (!value.contains("states") || !value["states"].is_object() || value["states"].empty()) {
        err = "policy needs a non-empty 'states' object";
        return false;
    }
    if (value.contains("max_transitions")) {
        if (!value["max_transitions"].is_number_integer() ||
            value["max_transitions"].get<long>() < 1) {
            err = "'max_transitions' must be a positive integer";
            return false;
        }
        spec.max_transitions = value["max_transitions"].get<int>();
        spec.bounded = true;
    }
    return true;
}

bool parse_on_error(const agent::json& value, Spec& spec, std::string& err) {
    if (!value.contains("on_error"))
        return true;
    const agent::json& on_error = value["on_error"];
    if (!on_error.is_object()) {
        err = "'on_error' must be an object";
        return false;
    }
    if (on_error.contains("retry")) {
        if (!on_error["retry"].is_number_integer() || on_error["retry"].get<long>() < 0) {
            err = "'on_error.retry' must be a non-negative integer";
            return false;
        }
        spec.retries = on_error["retry"].get<int>();
    }
    if (on_error.contains("next")) {
        if (!on_error["next"].is_string() || on_error["next"].get<std::string>().empty()) {
            err = "'on_error.next' must be a non-empty string";
            return false;
        }
        spec.on_error_next = on_error["next"].get<std::string>();
    }
    if (spec.retries > 0 && spec.on_error_next.empty()) {
        err = "'on_error.retry' needs 'on_error.next'";
        return false;
    }
    return true;
}

bool parse_states(const agent::json& value, Spec& spec, std::string& err) {
    for (auto it = value["states"].begin(); it != value["states"].end(); ++it) {
        State state;
        if (!parse_state(it.value(), state, err)) {
            std::string detail = std::move(err);
            err = it.key();
            err += ": ";
            err += detail;
            return false;
        }
        spec.states.emplace(it.key(), std::move(state));
    }
    return true;
}

bool validate_policy_edges(const Spec& spec, std::string& err) {
    if (spec.states.find(spec.initial) == spec.states.end()) {
        err = std::string("unknown initial state: ").append(spec.initial);
        return false;
    }
    for (const auto& [name, state] : spec.states) {
        if (state.next.empty())
            continue;
        if (spec.states.find(state.next) != spec.states.end())
            continue;
        err = std::string("state ").append(name).append(" points to an unknown state: ");
        err += state.next;
        return false;
    }
    if (!spec.on_error_next.empty() && spec.states.find(spec.on_error_next) == spec.states.end()) {
        err = std::string("on_error points to an unknown state: ").append(spec.on_error_next);
        return false;
    }
    return true;
}

bool parse_spec(const agent::json& value, Spec& spec, std::string& err) {
    if (!parse_policy_header(value, spec, err))
        return false;
    if (!parse_on_error(value, spec, err))
        return false;
    if (!parse_states(value, spec, err))
        return false;
    return validate_policy_edges(spec, err);
}

bool condition_holds(const agent::json& when, const PolicyContext& context) {
    if (!when.is_object() || when.empty())
        return true;
    for (auto it = when.begin(); it != when.end(); ++it) {
        const std::string& key = it.key();
        if (key == "always" && !it.value().get<bool>())
            return false;
        if (key == "last_tool_status" && context.last_tool_status != it.value().get<std::string>())
            return false;
        if (key == "last_tool_name" && context.last_tool_name != it.value().get<std::string>())
            return false;
        if (key == "last_result_contains" &&
            context.last_tool_result.find(it.value().get<std::string>()) == std::string::npos)
            return false;
        if (key == "step" && context.step != it.value().get<int>())
            return false;
    }
    return true;
}

bool has_reachable_terminal(const Spec& spec) {
    std::set<std::string> seen;
    std::function<bool(const std::string&)> visit = [&](const std::string& name) {
        if (!seen.insert(name).second)
            return false;
        const auto it = spec.states.find(name);
        if (it == spec.states.end())
            return false;
        if (it->second.kind != StateKind::Call)
            return true;
        if (visit(it->second.next))
            return true;
        if (!it->second.when.empty() && !spec.on_error_next.empty())
            return visit(spec.on_error_next);
        return false;
    };
    return visit(spec.initial);
}

bool has_cycle(const Spec& spec, std::string& err) {
    std::map<std::string, int> color;
    std::function<bool(const std::string&)> visit = [&](const std::string& name) {
        const int state = color[name];
        if (state == 1)
            return true;
        if (state == 2)
            return false;
        color[name] = 1;
        const auto it = spec.states.find(name);
        if (it != spec.states.end()) {
            if (!it->second.next.empty() && visit(it->second.next))
                return true;
            if (!it->second.when.empty() && !spec.on_error_next.empty() &&
                visit(spec.on_error_next))
                return true;
        }
        color[name] = 2;
        return false;
    };
    if (visit(spec.initial)) {
        err = "policy contains an unbounded cycle; set 'max_transitions'";
        return true;
    }
    return false;
}

class StateMachinePolicy : public Policy {
public:
    explicit StateMachinePolicy(Spec spec) : spec_(std::move(spec)), current_(spec_.initial) {}

    PolicyReply next(const PolicyContext& context) override {
        for (;;) {
            if (transitions_ >= spec_.max_transitions)
                return failure("policy transition cap exceeded");
            ++transitions_;
            const auto it = spec_.states.find(current_);
            if (it == spec_.states.end())
                return failure("unknown policy state: " + current_);
            const State& state = it->second;
            if (!condition_holds(state.when, context)) {
                if (retries_ >= spec_.retries || spec_.on_error_next.empty())
                    return failure("policy condition not met in state: " + current_);
                ++retries_;
                current_ = spec_.on_error_next;
                continue;
            }
            if (state.kind == StateKind::Final) {
                PolicyReply reply;
                reply.content = state.text;
                return reply;
            }
            if (state.kind == StateKind::Fail) {
                PolicyReply reply;
                reply.error = state.text;
                reply.retryable = false;
                return reply;
            }
            PolicyReply reply;
            reply.tool_calls = tool_call(state.call, context.step);
            current_ = state.next;
            return reply;
        }
    }

private:
    static PolicyReply failure(std::string message) {
        PolicyReply reply;
        reply.error = std::move(message);
        reply.retryable = false;
        return reply;
    }

    static agent::json tool_call(const agent::json& call, int step) {
        const std::string tool = call.value("tool", std::string());
        const agent::json args = call.contains("args") ? call["args"] : agent::json::object();
        const std::string arguments = args.is_string() ? args.get<std::string>() : args.dump();
        agent::json function = {{"name", tool}, {"arguments", arguments}};
        agent::json call_json = {
            {"id", "policy-" + std::to_string(step)}, {"type", "function"}, {"function", function}};
        agent::json calls = agent::json::array();
        calls.push_back(std::move(call_json));
        return calls;
    }

    Spec spec_;
    std::string current_;
    int transitions_ = 0;
    int retries_ = 0;
};

} // namespace

bool validate_state_machine(const agent::json& spec, std::string& err) {
    Spec parsed;
    if (!parse_spec(spec, parsed, err))
        return false;
    if (!has_reachable_terminal(parsed)) {
        err = "policy has no reachable terminal state";
        return false;
    }
    if (!parsed.bounded && has_cycle(parsed, err))
        return false;
    return true;
}

std::unique_ptr<Policy> make_state_machine_policy(const agent::json& spec, std::string& err) {
    Spec parsed;
    if (!parse_spec(spec, parsed, err))
        return nullptr;
    if (!has_reachable_terminal(parsed)) {
        err = "policy has no reachable terminal state";
        return nullptr;
    }
    if (!parsed.bounded && has_cycle(parsed, err))
        return nullptr;
    return std::make_unique<StateMachinePolicy>(std::move(parsed));
}

} // namespace bench
