#include "agent/agent.h"
#include "agent/tool.h"
#include "tests/fake_llm.h"
#include "tests/test_util.h"

#include <type_traits>

namespace {

agent::Config port_cfg() {
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 8;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    return cfg;
}

agent::AgentDeps client_deps(std::unique_ptr<agent::LLMClient> client) {
    agent::AgentDeps deps;
    deps.client = std::move(client);
    return deps;
}

// Implements only the telemetry role.
struct UsageOnly : agent::ITelemetrySink {
    int stats = 0;
    int usage = 0;
    void on_stats(const agent::Stats&) override { ++stats; }
    void on_usage(const agent::UsageSample&) override { ++usage; }
};

// Implements only the approval role.
struct AllowProbe : agent::IToolApproval {
    int calls = 0;
    agent::Approval approve(const std::string&, const agent::json&, const std::string&) override {
        ++calls;
        return agent::Approval::AllowOnce;
    }
};

class GatedProbe : public agent::Tool {
public:
    explicit GatedProbe(bool* ran) : ran_(ran) {}

    std::string name() const noexcept override { return "gated_probe"; }
    std::string description() const noexcept override { return "a gated probe"; }
    agent::json parameters_schema() const override { return {{"type", "object"}}; }
    bool requires_approval(const agent::json&) const noexcept override { return true; }
    agent::ToolResult execute(const agent::json&) const override {
        *ran_ = true;
        return agent::ToolResult{true, "ran", "", agent::json::object()};
    }

private:
    bool* ran_;
};

void push_tool_call(const std::shared_ptr<std::deque<agent_test::FakeReply>>& script,
                    const std::string& name) {
    agent_test::FakeReply call;
    call.tool_calls = agent::json::array({{{"id", "call_1"},
                                           {"type", "function"},
                                           {"function", {{"name", name}, {"arguments", "{}"}}}}});
    script->push_back(std::move(call));
}

} // namespace

// A host that needs usage telemetry implements one role; the untouched roles
// stay empty rather than forcing eleven no-op lambdas.
TEST(agent_hooks_role_host_receives_only_its_telemetry) {
    UsageOnly sink;
    agent::AgentHooks hooks(nullptr, nullptr, nullptr, &sink);
    ASSERT_FALSE(hooks.on_approval);
    ASSERT_FALSE(hooks.on_debug);
    ASSERT_TRUE(hooks.on_usage);

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply reply;
    reply.content = "done";
    reply.prompt_tokens = 12;
    reply.completion_tokens = 3;
    fake->script.push_back(std::move(reply));
    agent::ToolRegistry reg;
    agent::Agent ag(port_cfg(), reg, hooks, client_deps(std::move(fake)));
    ASSERT_EQ(ag.run("go"), "done");
    ASSERT_EQ(sink.stats, 1);
    ASSERT_EQ(sink.usage, 1);
}

// The approval role is the only source of a grant; a host that installs none
// keeps the fail-closed default.
TEST(agent_hooks_role_approval_grants_and_absence_denies) {
    agent::ToolRegistry reg;
    bool ran = false;
    reg.register_tool(std::make_unique<GatedProbe>(&ran));

    AllowProbe approval;
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "gated_probe");
    agent_test::FakeReply done;
    done.content = "done";
    script->push_back(std::move(done));
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    fake->script = *script;
    agent::Agent ag(port_cfg(), reg, agent::AgentHooks(nullptr, &approval),
                    client_deps(std::move(fake)));
    ASSERT_EQ(ag.run("run the probe"), "done");
    ASSERT_EQ(approval.calls, 1);
    ASSERT(ran);

    ran = false;
    auto denied_script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(denied_script, "gated_probe");
    agent_test::FakeReply denied_done;
    denied_done.content = "done";
    denied_script->push_back(std::move(denied_done));
    auto denied_fake = std::make_unique<agent_test::FakeLLMClient>();
    denied_fake->script = *denied_script;
    agent::Agent denied(port_cfg(), reg, agent::AgentHooks{}, client_deps(std::move(denied_fake)));
    ASSERT_EQ(denied.run("run the probe"), "done");
    ASSERT_FALSE(ran);
}

// Hosts hold the run port, not the concrete engine.
TEST(agent_port_runs_and_snapshots_without_the_concrete_type) {
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply reply;
    reply.content = "via the port";
    fake->script.push_back(std::move(reply));
    agent::ToolRegistry reg;
    agent::Agent concrete(port_cfg(), reg, {}, client_deps(std::move(fake)));
    agent::IAgent& port = concrete;
    const auto snapshot = port.config_snapshot();
    ASSERT_TRUE(snapshot != nullptr);
    ASSERT_EQ(snapshot->model, port_cfg().model);
    ASSERT_EQ(port.run("go"), "via the port");
}

// The constructor takes Config, registry, hooks and one dependency bundle.
TEST(agent_constructor_is_below_the_parameter_cap) {
    static_assert(std::is_constructible<agent::Agent, agent::Config, agent::ToolRegistry&,
                                        agent::AgentHooks, agent::AgentDeps>::value,
                  "the AgentDeps constructor must exist");
    static_assert(
        !std::is_constructible<agent::Agent, agent::Config, agent::ToolRegistry&, agent::AgentHooks,
                               std::unique_ptr<agent::CompressionStrategy>>::value,
        "the old wide constructor must be gone");
}
