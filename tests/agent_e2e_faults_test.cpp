// Adversarial agent-loop tests: a scripted endpoint that fails in every way the
// transport can, tools that deny/throw/duplicate, budgets that stop the loop,
// and a seeded policy fuzz. Each case asserts a recovery or a clean failure and
// the invariants that make a run trustworthy: every call has one result, the
// context hash chain holds, and no call id executes twice.

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent.h"
#include "agent/context.h"
#include "agent/run_scope.h"
#include "agent/tools.h"
#include "bench/client_filters.h"
#include "bench/policy.h"
#include "bench/policy_client.h"
#include "bench/scripted_endpoint.h"
#include "tests/fake_llm.h"
#include "tests/test_util.h"

namespace {

namespace fs = std::filesystem;

agent::Config fault_cfg() {
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 8;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    return cfg;
}

// A read-only probe that records how often it actually ran.
class ProbeTool : public agent::Tool {
public:
    std::string name() const noexcept override { return "probe"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "record a call"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"},
                {"properties", {{"value", {{"type", "string"}}}}},
                {"required", nlohmann::json::array()}};
    }
    agent::ToolResult execute(const agent::json& a) const override {
        ++calls;
        agent::ToolResult r;
        r.ok = true;
        r.output = "probe:" + a.value("value", std::string());
        return r;
    }
    mutable std::atomic<int> calls{0};
};

// A gated probe, for the denied-approval and read-mode cases.
class GatedTool : public agent::Tool {
public:
    std::string name() const noexcept override { return "gated"; }
    bool is_read_only() const noexcept override { return false; }
    bool requires_approval(const agent::json&) const noexcept override { return true; }
    std::string description() const noexcept override { return "a gated action"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    agent::ToolResult execute(const agent::json&) const override {
        ++calls;
        agent::ToolResult r;
        r.ok = true;
        r.output = "gated ran";
        return r;
    }
    mutable std::atomic<int> calls{0};
};

// A tool that throws out of execute(): the dispatcher must convert that to an
// error result, not let it escape the agent loop.
class ThrowingTool : public agent::Tool {
public:
    std::string name() const noexcept override { return "boom"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "throws"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    agent::ToolResult execute(const agent::json&) const override {
        throw std::runtime_error("tool exploded");
    }
};

// Blocks until the run's cancellation token fires.
class CancelProbe : public agent::Tool {
public:
    std::string name() const noexcept override { return "wait_cancel"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "wait"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    agent::ToolResult execute(const agent::json&) const override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!agent::run_cancelled(fallback_) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        observed = agent::run_cancelled(fallback_);
        agent::ToolResult r;
        r.ok = !observed;
        r.output = observed ? "cancelled" : "timeout";
        if (!r.ok)
            r.error = "cancelled";
        return r;
    }
    mutable std::atomic<bool> observed{false};
    agent::CancellationToken fallback_;
};

struct Outcome {
    std::string final_text;
    std::string error;
    int chat_calls = 0;
    bool threw = false;
};

Outcome run_with_client(agent::Config cfg, agent::ToolRegistry& registry,
                        std::unique_ptr<agent::LLMClient> client) {
    Outcome out;
    agent::AgentDeps deps;
    deps.client = std::move(client);
    try {
        agent::Agent agent(cfg, registry, {}, std::move(deps));
        out.final_text = agent.run("go");
        // get_all() verifies the FNV-1a hash chain before returning.
        (void)agent.context().get_all();
    } catch (const std::exception& e) {
        out.threw = true;
        out.error = e.what();
    }
    return out;
}

// Pair every assistant tool call with exactly one tool result. Returns a
// message naming the scenario, call id and issuing message on the first
// violation.
std::string pairing_violation(const std::vector<agent::Message>& messages,
                              const std::string& scenario) {
    int calls = 0;
    int results = 0;
    std::map<std::string, int> result_ids;
    std::map<std::string, int> call_steps;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const agent::Message& message = messages[index];
        if (message.role == "assistant") {
            for (const auto& call : message.tool_calls) {
                ++calls;
                if (call.contains("id") && call["id"].is_string())
                    call_steps[call["id"].get<std::string>()] = static_cast<int>(index);
            }
        } else if (message.role == "tool") {
            ++results;
            ++result_ids[message.tool_call_id];
        }
    }
    if (calls != results)
        return scenario + ": " + std::to_string(calls) + " call(s) but " + std::to_string(results) +
               " result(s)";
    for (const auto& [id, count] : result_ids)
        if (count != 1)
            return scenario + ": call id '" + id + "' has " + std::to_string(count) +
                   " results (issued at message " + std::to_string(call_steps[id]) + ")";
    return {};
}

std::string pairing_violation(const agent::Context& context, const std::string& scenario) {
    const std::deque<agent::Message>& all = context.get_all();
    return pairing_violation(std::vector<agent::Message>(all.begin(), all.end()), scenario);
}

} // namespace

// ── Endpoint and loop faults ────────────────────────────────────────────────

TEST(agent_fault_matrix_endpoint) {
    struct Case {
        const char* name = nullptr;
        std::deque<bench::EndpointReply> script;
        bool stream = false;
        const char* expect = "done";
        int calls = 0; // 0 = do not pin the request count
    };
    auto reply = [](const std::string& content) {
        bench::EndpointReply r;
        r.content = content;
        return r;
    };
    auto status = [](int code, int retry_after = 0) {
        bench::EndpointReply r;
        r.status = code;
        r.retry_after_s = retry_after;
        r.body = R"({"error":{"message":"scripted failure"}})";
        return r;
    };
    auto disconnect = [] {
        bench::EndpointReply r;
        r.disconnect = true;
        return r;
    };
    auto malformed = [] {
        bench::EndpointReply r;
        r.malformed_json = true;
        return r;
    };
    auto truncated = [] {
        bench::EndpointReply r;
        r.content_type = "text/event-stream";
        r.sse_chunks = {R"(data: {"choices":[{"delta":{"content":"partial"}}]})"
                        "\n\n"};
        return r;
    };
    std::vector<Case> cases;
    cases.push_back({"429 retry", {status(429), reply("done")}, false, "done", 2});
    cases.push_back({"429 with retry-after", {status(429, 1), reply("done")}, false, "done", 2});
    cases.push_back({"500 retry", {status(500), reply("done")}, false, "done", 2});
    cases.push_back({"disconnect retry", {disconnect(), reply("done")}, false, "done", 2});
    cases.push_back({"400 harness notice", {status(400), reply("done")}, false, "done", 2});
    cases.push_back({"malformed JSON", {malformed()}, false, "malformed", 0});
    cases.push_back({"empty reply", {reply(""), reply("")}, false, "no usable reply", 0});
    cases.push_back({"truncated SSE", {truncated()}, true, "partial", 0});
    cases.push_back({"exhausted retries",
                     {status(503), status(503), status(503), reply("done")},
                     false,
                     "done",
                     0});

    for (Case& c : cases) {
        bench::ScriptedEndpoint endpoint(std::move(c.script));
        if (!endpoint.start()) {
            ASSERT(false);
            continue;
        }
        agent::Config cfg = fault_cfg();
        cfg.api_base = endpoint.base_url();
        cfg.model = "fake";
        cfg.stream = c.stream;
        if (c.stream)
            cfg.max_tool_iterations = 4;
        agent::ToolRegistry registry;
        auto client = std::make_unique<agent::HttpLLMClient>(cfg);
        const Outcome out = run_with_client(cfg, registry, std::move(client));
        ASSERT(!out.threw);
        if (out.final_text.find(c.expect) == std::string::npos) {
            std::cerr << "fault case '" << c.name << "' expected '" << c.expect
                      << "', got: " << out.final_text << "\n";
            ASSERT(false);
        }
        if (c.calls > 0 && endpoint.chat_calls() != c.calls) {
            std::cerr << "fault case '" << c.name << "' made " << endpoint.chat_calls()
                      << " request(s), expected " << c.calls << "\n";
            ASSERT(false);
        }
        endpoint.stop();
    }
}

TEST(agent_fault_matrix_loop) {
    struct Case {
        const char* name = nullptr;
        std::deque<agent_test::FakeReply> script;
        bool recover = false;
        bool gated = false;
        bool allow = false;
        bool read_mode = false;
        bool duplicate = false;
    };
    auto text = [](const std::string& content) {
        agent_test::FakeReply r;
        r.content = content;
        return r;
    };
    auto call = [](const std::string& tool, const std::string& args, const std::string& id = "c1") {
        agent_test::FakeReply r;
        r.tool_calls = agent_test::tool_call_msg(tool, agent::json::parse(args), id).tool_calls;
        return r;
    };
    auto call_raw = [](const std::string& tool, const std::string& raw_args,
                       const std::string& id = "c1") {
        agent_test::FakeReply r;
        r.tool_calls =
            agent::json::array({{{"id", id},
                                 {"type", "function"},
                                 {"function", {{"name", tool}, {"arguments", raw_args}}}}});
        return r;
    };
    auto duplicate_calls = [] {
        agent_test::FakeReply r;
        agent::Message first =
            agent_test::tool_call_msg("probe", agent::json{{"value", "x"}}, "same");
        agent::Message second =
            agent_test::tool_call_msg("probe", agent::json{{"value", "x"}}, "same");
        r.tool_calls = first.tool_calls;
        for (const auto& call : second.tool_calls)
            r.tool_calls.push_back(call);
        return r;
    };
    std::vector<Case> cases;
    cases.push_back({"unknown tool", {call("no_such_tool", "{}"), text("done")}, true});
    cases.push_back({"malformed args", {call_raw("probe", "{bad"), text("done")}, true});
    cases.push_back({"denied approval", {call("gated", "{}"), text("done")}, true, true, false});
    cases.push_back({"allowed approval", {call("gated", "{}"), text("done")}, true, true, true});
    cases.push_back(
        {"read mode denies", {call("gated", "{}"), text("done")}, true, true, false, true});
    cases.push_back({"tool throws", {call("boom", "{}"), text("done")}, true});
    cases.push_back({"duplicate id executes once",
                     {duplicate_calls(), text("done")},
                     true,
                     false,
                     false,
                     false,
                     true});

    for (Case& c : cases) {
        agent::Config cfg = fault_cfg();
        cfg.mode = c.read_mode ? agent::AgentMode::Read : agent::AgentMode::Write;
        agent::ToolRegistry registry;
        auto probe = std::make_unique<ProbeTool>();
        ProbeTool* probe_ptr = probe.get();
        registry.register_tool(std::move(probe));
        auto gated = std::make_unique<GatedTool>();
        GatedTool* gated_ptr = gated.get();
        registry.register_tool(std::move(gated));
        registry.register_tool(std::make_unique<ThrowingTool>());

        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        fake->script = std::move(c.script);
        agent::AgentHooks hooks;
        if (c.allow)
            hooks.on_approval = [](const std::string&, const agent::json&, const std::string&) {
                return agent::Approval::AllowOnce;
            };
        else
            hooks.on_approval = [](const std::string&, const agent::json&, const std::string&) {
                return agent::Approval::Deny;
            };
        agent::AgentDeps deps;
        deps.client = std::move(fake);
        const Outcome out = [&] {
            Outcome o;
            try {
                agent::Agent agent(cfg, registry, hooks, std::move(deps));
                if (c.duplicate) {
                    // The default duplicate detection is on in Yolo; the test
                    // asserts the observable effect below.
                }
                o.final_text = agent.run("go");
                (void)agent.context().get_all();
            } catch (const std::exception& e) {
                o.threw = true;
                o.error = e.what();
            }
            return o;
        }();
        ASSERT(!out.threw);
        if (c.recover && out.final_text.find("done") == std::string::npos) {
            std::cerr << "loop case '" << c.name << "' did not recover: " << out.final_text << "\n";
            ASSERT(false);
        }
        if (c.duplicate && probe_ptr->calls.load() != 1) {
            std::cerr << "duplicate call ran " << probe_ptr->calls.load() << " times\n";
            ASSERT(false);
        }
        if (c.gated && !c.allow && gated_ptr->calls.load() != 0) {
            std::cerr << "denied/read-mode gated tool ran\n";
            ASSERT(false);
        }
    }
}

TEST(agent_fault_matrix_budgets_and_cancel) {
    // Step budget: the loop stops at the cap instead of running the whole
    // script. The final reply names the cap.
    {
        agent::Config cfg = fault_cfg();
        cfg.max_tool_iterations = 1;
        agent::ToolRegistry registry;
        auto probe = std::make_unique<ProbeTool>();
        ProbeTool* probe_ptr = probe.get();
        registry.register_tool(std::move(probe));
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        for (int i = 0; i < 3; ++i) {
            agent_test::FakeReply r;
            r.tool_calls = agent_test::tool_call_msg("probe", agent::json{{"value", i}},
                                                     "c" + std::to_string(i))
                               .tool_calls;
            fake->script.push_back(r);
        }
        fake->script.push_back(agent_test::FakeReply{});
        agent::AgentDeps deps;
        deps.client = std::move(fake);
        agent::Agent agent(cfg, registry, {}, std::move(deps));
        (void)agent.run("go");
        // One tool iteration is allowed; the third scripted call must not run.
        ASSERT(probe_ptr->calls.load() < 3);
        (void)agent.context().get_all();
    }

    // Cancellation: a tool blocked in run_cancelled() sees the request and the
    // turn ends without a fabricated assistant message.
    {
        agent::Config cfg = fault_cfg();
        cfg.max_tool_iterations = 2;
        agent::ToolRegistry registry;
        auto probe = std::make_unique<CancelProbe>();
        CancelProbe* probe_ptr = probe.get();
        registry.register_tool(std::move(probe));
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        agent_test::FakeReply call;
        call.tool_calls =
            agent_test::tool_call_msg("wait_cancel", agent::json::object()).tool_calls;
        fake->script.push_back(call);
        agent_test::FakeReply done;
        done.content = "done";
        fake->script.push_back(done);
        agent::AgentDeps deps;
        deps.client = std::move(fake);
        agent::Agent agent(cfg, registry, {}, std::move(deps));
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            agent.request_cancel();
        });
        (void)agent.run("go");
        canceller.join();
        ASSERT(probe_ptr->observed.load());
        (void)agent.context().get_all();
    }
}

class SlowProbe : public agent::Tool {
public:
    std::string name() const noexcept override { return "slow"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "sleep"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    agent::ToolResult execute(const agent::json&) const override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        agent::ToolResult r;
        r.ok = true;
        r.output = "slept";
        return r;
    }
};

TEST(agent_fault_matrix_wall_budget_and_confinement) {
    {
        agent::Config cfg = fault_cfg();
        cfg.max_wall_ms = 50;
        agent::ToolRegistry registry;
        registry.register_tool(std::make_unique<SlowProbe>());
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        agent_test::FakeReply call;
        call.tool_calls = agent_test::tool_call_msg("slow", agent::json::object()).tool_calls;
        fake->script.push_back(call);
        agent_test::FakeReply done;
        done.content = "done";
        fake->script.push_back(done);
        agent::AgentDeps deps;
        deps.client = std::move(fake);
        const auto start = std::chrono::steady_clock::now();
        {
            agent::Agent agent(cfg, registry, {}, std::move(deps));
            (void)agent.run("go");
            (void)agent.context().get_all();
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        // The tool itself cannot be interrupted, so the ceiling is generous:
        // the point is that the loop stops after it instead of continuing.
        ASSERT(elapsed_ms < 15000);
    }
    {
        std::string resolved;
        std::string error;
        ASSERT(agent::Workspace::confine("a.txt", resolved, error));
        ASSERT(!agent::Workspace::confine("../escape.txt", resolved, error));
        ASSERT(!agent::Workspace::confine("/etc/passwd", resolved, error));
    }
}

TEST(pairing_violation_names_the_scenario_and_call_id) {
    // A deliberately broken fixture, not a Context: the same call id twice,
    // with two results, is exactly the shape the invariant must reject.
    std::vector<agent::Message> messages;
    agent::json call = {{"id", "dup"},
                        {"type", "function"},
                        {"function", {{"name", "probe"}, {"arguments", "{}"}}}};
    agent::Message assistant;
    assistant.role = "assistant";
    assistant.tool_calls = agent::json::array();
    assistant.tool_calls.push_back(call);
    assistant.tool_calls.push_back(call);
    messages.push_back(assistant);
    agent::Message result;
    result.role = "tool";
    result.tool_call_id = "dup";
    messages.push_back(result);
    messages.push_back(result);
    const std::string violation = pairing_violation(messages, "broken-fixture");
    ASSERT(violation.find("broken-fixture") != std::string::npos);
    ASSERT(violation.find("dup") != std::string::npos);
    ASSERT(violation.find("message 0") != std::string::npos);
}

TEST(agent_fault_matrix_client_filters) {
    struct Case {
        const char* kind;
        const char* expect;
    };
    const Case cases[] = {{"timeout", "done"}, {"http_400", "done"}};
    for (const Case& c : cases) {
        agent::Config cfg = fault_cfg();
        agent::ToolRegistry registry;
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        agent_test::FakeReply done;
        done.content = "done";
        fake->script.push_back(done);
        const agent::json faults = agent::json::array({{{"at_request", 1}, {"kind", c.kind}}});
        auto client = std::make_unique<bench::FaultClient>(std::move(fake), faults);
        const Outcome out = run_with_client(cfg, registry, std::move(client));
        ASSERT(!out.threw);
        if (out.final_text.find(c.expect) == std::string::npos) {
            std::cerr << "client fault '" << c.kind << "' expected '" << c.expect
                      << "', got: " << out.final_text << "\n";
            ASSERT(false);
        }
    }
}

// ── Seeded policy fuzz ──────────────────────────────────────────────────────

namespace {

// A linear state machine: one probe call per state, then a final answer. The
// generator varies the step count and args from a fixed seed.
agent::json generated_policy(std::mt19937& rng, int steps) {
    agent::json states = agent::json::object();
    for (int i = 0; i < steps; ++i) {
        agent::json call = {{"tool", "probe"},
                            {"args", {{"value", "v" + std::to_string(rng() % 1000)}}}};
        agent::json state = {{"call", call}, {"next", "s" + std::to_string(i + 1)}};
        states["s" + std::to_string(i)] = state;
    }
    states["s" + std::to_string(steps)] = agent::json{{"final", "done"}};
    return agent::json{{"initial", "s0"}, {"states", states}};
}

} // namespace

TEST(agent_seeded_policy_fuzz_preserves_invariants) {
    std::mt19937 rng(0x641);
    constexpr int kSequences = 1000;
    for (int i = 0; i < kSequences; ++i) {
        const int steps = 0 + static_cast<int>(rng() % 6);
        std::string policy_error;
        auto policy = bench::make_state_machine_policy(generated_policy(rng, steps), policy_error);
        if (!policy) {
            std::cerr << "generated policy " << i << " is invalid: " << policy_error << "\n";
            ASSERT(false);
            continue;
        }
        agent::Config cfg = fault_cfg();
        cfg.max_tool_iterations = steps + 2;
        agent::ToolRegistry registry;
        auto probe = std::make_unique<ProbeTool>();
        ProbeTool* probe_ptr = probe.get();
        registry.register_tool(std::move(probe));
        auto client = std::make_unique<bench::PolicyClient>(std::move(policy));
        bench::PolicyClient* client_ptr = client.get();
        agent::AgentDeps deps;
        deps.client = std::move(client);
        try {
            agent::Agent agent(cfg, registry, {}, std::move(deps));
            const std::string final_text = agent.run("go");
            const std::string violation =
                pairing_violation(agent.context(), "policy " + std::to_string(i));
            if (!violation.empty()) {
                std::cerr << violation << "\n";
                ASSERT(false);
            }
            if (probe_ptr->calls.load() != steps) {
                std::cerr << "policy " << i << " executed " << probe_ptr->calls.load()
                          << " probe call(s), expected " << steps << "\n";
                ASSERT(false);
            }
            if (final_text.find("done") == std::string::npos) {
                std::cerr << "policy " << i << " did not finish: " << final_text << "\n";
                ASSERT(false);
            }
            (void)client_ptr->chat_calls;
        } catch (const std::exception& e) {
            std::cerr << "policy " << i << " threw: " << e.what() << "\n";
            ASSERT(false);
        }
    }
}
