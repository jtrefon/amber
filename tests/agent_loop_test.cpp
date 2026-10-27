
// Hermetic agent-loop tests: the real Agent + real Context + real registry,
// driven by the scripted FakeLLMClient. No network, no server.
// Scenarios map to docs/spec/llm-client/agent-loop-reliability.md [AL-xx].

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <future>
#include <thread>
#include <unistd.h>
#include <atomic>

#include "agent.h"
#include "agent/agent_helpers.h"
#include "agent/rate_governor.h"
#include "agent/tools.h"
#include "agent/plugins_bundled.h"
#include "agent/plugin_runtime.h"
#include "agent/todo.h"
#include "agent/session_brief.h"
#include "agent/run_scope.h"
#include "agent/worktree.h"
#include "fake_llm.h"
#include "tests/test_util.h"

namespace {

std::string cwd() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : ".";
}

agent::ModelCapabilities caps(int context = 0) {
    agent::ModelCapabilities c;
    c.context = context;
    return c;
}

agent::Config loop_cfg() {
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 100;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    return cfg;
}

// A scratch workspace carrying one authored skill, so the discovery block has
// something to list. The caller makes it the workspace root for the test.
std::string workspace_with_one_skill(const std::string& name) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("amber_skill_gate_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir / ".amber" / "skills" / "demo");
    std::ofstream skill(dir / ".amber" / "skills" / "demo" / "SKILL.md");
    skill << "---\nname: demo\ndescription: a demo skill\n---\nBody.\n";
    return dir.string();
}

// Hooks that auto-approve every approval-gated tool, for tests that exercise
// side-effecting tools (write create, task, plugin) without a real UI.
agent::AgentHooks auto_approve_hooks() {
    agent::AgentHooks h;
    h.on_approval = [](const std::string&, const agent::json&, const std::string&) {
        return agent::Approval::AllowOnce;
    };
    return h;
}

void push_text(agent_test::FakeLLMClient& fake, const std::string& text) {
    agent_test::FakeReply r;
    r.content = text;
    fake.script.push_back(std::move(r));
}

void push_text(const std::shared_ptr<std::deque<agent_test::FakeReply>>& script,
               const std::string& text) {
    agent_test::FakeReply r;
    r.content = text;
    script->push_back(std::move(r));
}

void push_tool_call(const std::shared_ptr<std::deque<agent_test::FakeReply>>& script,
                    const std::string& fn, const json& args, const std::string& id = "call_1") {
    agent_test::FakeReply r;
    r.tool_calls = json::array({{{"id", id},
                                 {"type", "function"},
                                 {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    script->push_back(std::move(r));
}

void push_tool_call(agent_test::FakeLLMClient& fake, const std::string& fn, const json& args,
                    const std::string& id = "call_1") {
    agent_test::FakeReply r;
    r.tool_calls = json::array({{{"id", id},
                                 {"type", "function"},
                                 {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    fake.script.push_back(std::move(r));
}

// A registered tool for prefix tests: the schema never runs, only its presence
// in the compression request matters.
class PrefixProbeTool : public agent::Tool {
public:
    std::string name() const noexcept override { return "prefix_probe"; }
    std::string description() const noexcept override { return "prefix probe"; }
    agent::json parameters_schema() const override { return agent::json::object(); }
    agent::ToolResult execute(const agent::json&) const override { return {}; }
};

} // namespace

// [BENCH] A long read with and without the context budget: the tool message,
// and every later request that re-sends it, shrinks; the turn still completes.
TEST(tool_budget_reduces_resent_bytes) {
    namespace fs = std::filesystem;
    std::string tmpl = "/tmp/amber_budget_bench_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const char* made = mkdtemp(buf.data());
    ASSERT(made != nullptr);
    const std::string root = made;
    {
        std::ofstream f(root + "/big.log");
        ASSERT(f.good());
        for (int i = 0; i < 2000; ++i)
            f << "line " << i << " " << std::string(70, 'x') << "\n";
    }
    const auto measure = [&](int budget) -> std::size_t {
        agent::Workspace::set_root(root);
        agent::Config cfg = loop_cfg();
        cfg.log_path = root + "/transcript.jsonl";
        cfg.max_tool_result_bytes = budget;
        agent::ToolRegistry reg;
        agent::JobService jobs;
        agent::TodoStore todos;
        agent::SubAgentExecutor subagents;
        agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        agent_test::FakeLLMClient* raw = fake.get();
        push_tool_call(*fake, "read", {{"path", "big.log"}, {"limit", 2000}});
        push_text(*fake, "done");
        agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
        const std::string reply = ag.run("read the log");
        ASSERT_EQ(reply, std::string("done"));
        ASSERT(!raw->requests.empty());
        std::size_t bytes = 0;
        for (const auto& m : raw->requests.back())
            bytes += m.content.size();
        return bytes;
    };
    const std::size_t uncapped = measure(0);
    const std::size_t capped = measure(32 * 1024);
    std::printf("[bench] long-read resend bytes: uncapped=%zu capped=%zu\n", uncapped, capped);
    ASSERT(capped < uncapped);
    ASSERT(uncapped - capped > 100000u);
    fs::remove_all(root);
}

// [AL-01] A reply with no tool calls is the turn's answer: one generation, and
// the harness injects nothing behind it. The old contract asked "Are you
// finished?" and paid a second generation (and a probe) for every reply.
TEST(agent_loop_plain_reply) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "hello there");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "hello there");
    ASSERT_EQ(raw->chat_calls, 1); // one generation; the turn is over
    // Context: system, user, assistant — nothing else.
    const auto& ctx = ag.context().get_all();
    ASSERT_EQ(ctx.size(), 3u);
    // [I-9] The environment card rides in the system message.
    ASSERT(ctx[0].content.find("## Environment") != std::string::npos);
    ASSERT_EQ(ctx[1].role, "user");
    ASSERT_EQ(ctx[1].content, "hi");
    ASSERT_EQ(ctx[2].role, "assistant");
    ASSERT(!ctx[2].harness);
    ASSERT(!ctx[2].content.empty());
}

// [AL-02] A tool call round trip: model -> real tool -> result -> final text.
TEST(agent_loop_tool_roundtrip) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "done reading");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("read the Makefile");
    ASSERT_EQ(reply, "done reading");
    ASSERT_EQ(raw->chat_calls, 2); // tool call, then the closing reply
    // The tool result must be in the conversation (fed back to the model).
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" && m.content.find("Makefile") != std::string::npos)
            saw_tool_result = true;
    ASSERT(saw_tool_result);
    // Second request carries the tool message back to the model.
    ASSERT(raw->requests.size() >= 2u);
    bool fed_back = false;
    for (const auto& m : raw->requests[1])
        if (m.role == "tool")
            fed_back = true;
    ASSERT(fed_back);
}

// [AL-03] A reply carrying BOTH content and tool calls dispatches the calls
// and keeps the loop going (the use-after-move regression: moved-from
// tool_calls used to silently kill this path).
TEST(agent_loop_mixed_reply_dispatches_tools) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    agent_test::FakeReply mixed;
    mixed.content = "let me check";
    mixed.tool_calls = agent_test::tool_call_msg("read", {{"path", "Makefile"}}, "c1").tool_calls;
    fake->script.push_back(std::move(mixed));
    push_text(*fake, "verified");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("check the build file");
    ASSERT_EQ(reply, "verified");
    // The tool call was dispatched: a tool result is in context.
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" && m.content.find("Makefile") != std::string::npos)
            saw_tool_result = true;
    ASSERT(saw_tool_result);
    ASSERT(raw->chat_calls == 2);
}

// [AL-04] The loop stops at max_tool_iterations, and the stop is part of the
// record: the reason is pushed as a [harness] notice and announced to the host.
TEST(agent_loop_max_tool_iterations) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.max_tool_iterations = 3;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "read", {{"path", "Makefile"}}, "c1");
    push_tool_call(*fake, "read", {{"path", "Makefile.in"}}, "c2");
    push_tool_call(*fake, "read", {{"path", "lib/llm.cpp"}}, "c3");
    std::string announced;
    agent::AgentHooks hooks;
    hooks.on_assistant = [&announced](const std::string& text) { announced = text; };
    agent::Agent ag(cfg, reg, std::move(hooks), {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("read many files");
    ASSERT_FALSE(reply.empty()); // graceful fallback, not a crash
    ASSERT_EQ(raw->chat_calls, 3);
    ASSERT(raw->requests.size() == 3u);
    // The host is told: a stop the user never sees is a dead session.
    ASSERT_EQ(announced, reply);
    // And the model is told, in a notice, so the next turn opens knowing.
    bool noticed = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness && m.content.find("ended without a closing reply") != std::string::npos)
            noticed = true;
    ASSERT(noticed);
}

// [AL-16] A model that goes silent after tool calls gets the nudge, and when it
// stays silent the stop is reported like any other bound.
TEST(agent_loop_empty_after_tools_is_reported) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "read", {{"path", "Makefile"}}, "c1");
    for (int i = 0; i < 2; ++i)
        fake->script.push_back(agent_test::FakeReply{}); // silent, no tool calls
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("read the Makefile");
    ASSERT(reply.find("agent stopped") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 3); // the tool turn + two silent replies
    int nudges = 0;
    bool stopped = false;
    for (const auto& m : ag.context().get_all()) {
        if (!m.harness)
            continue;
        if (m.content.find("no tool call and no content") != std::string::npos)
            ++nudges;
        if (m.content.find("ended without a closing reply") != std::string::npos)
            stopped = true;
    }
    ASSERT_EQ(nudges, 1); // the silence is named once, then the turn stops
    ASSERT(stopped);
}

// [AL-05] Repeated narration while the model keeps calling tools triggers the
// opt-in steer, then the hard stop.
TEST(agent_loop_text_loop_detection) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.detection_loop = true;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    bool steered = false;
    agent::AgentHooks hooks;
    hooks.on_status = [&](const std::string& s) {
        if (s.find("text loop") != std::string::npos)
            steered = true;
    };
    // Six iterations that narrate "foo" and call a DIFFERENT tool each time, so
    // the tool-loop detector stays quiet and the text guard is what fires: the
    // steer at repeat 2, the hard stop at repeat 6.
    const char* paths[] = {"Makefile",      "Makefile.in",         "lib/llm.cpp",
                           "lib/agent.cpp", "tests/run_tests.cpp", "include/agent/agent.h"};
    for (int i = 0; i < 6; ++i) {
        agent_test::FakeReply r;
        r.content = "foo";
        r.tool_calls =
            agent_test::tool_call_msg("read", {{"path", paths[i]}}, "c" + std::to_string(i))
                .tool_calls;
        fake->script.push_back(std::move(r));
    }
    agent::Agent ag(cfg, reg, std::move(hooks), {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("do the thing");
    ASSERT(steered);
    ASSERT(reply.find("loop detected") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 6); // 6th repeat hard-stops the turn
}

// [AL-06] The compression gate fires once the cooldown window passes; the
// context is rebuilt via clear+push and the hash chain stays intact.
TEST(agent_loop_compression_trigger) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 2000;
    cfg.compression_threshold = 0.5;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    // Warm-up turns (cooldown default is 20 turns before the gate opens).
    for (int i = 0; i < 21; ++i)
        push_text(*fake, "warm");
    // The compression turn: 1) classify, 2) extract, 3) generation.
    {
        agent_test::FakeReply r;
        r.content = R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
                    R"("memories":[],"skills":[]})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "hello after compression");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i)
        ag.run("warm " + std::to_string(i));

    std::string big_prompt(8000, 'x');
    std::string reply = ag.run(big_prompt);
    ASSERT_EQ(reply, "hello after compression");
    // The classification request reached the client somewhere in the flow.
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("CLASSIFICATION of every older turn range") != std::string::npos)
                saw_compression = true;
    ASSERT(saw_compression);
    // Hash chain intact after the clear+push rebuild.
    (void)ag.context().get_all();
}

TEST(retry_after_parses_seconds_not_dates) {
    ASSERT_EQ(agent::parse_retry_after_ms("30"), 30000);
    ASSERT_EQ(agent::parse_retry_after_ms(" 7 \r\n"), 7000);
    ASSERT_EQ(agent::parse_retry_after_ms("120"), 120000);
    ASSERT_EQ(agent::parse_retry_after_ms("Wed, 21 Oct 2015 07:28:00 GMT"), 0);
    ASSERT_EQ(agent::parse_retry_after_ms(""), 0);
}

// A 429 is a pause, not a dead request: the next attempt is made after the
// governor's wait and can succeed. The old behavior stopped after one.
TEST(chat_with_retry_retries_a_rate_limit) {
    agent::ConversationLog log;
    agent::AgentHooks hooks;
    agent::CancellationToken token;
    int calls = 0;
    auto chat = [&calls]() -> agent::Message {
        if (++calls == 1)
            throw agent::ApiError(429, true, "HTTP 429: rate limited", 100);
        agent::Message m;
        m.role = "assistant";
        m.content = "ok";
        return m;
    };
    const agent::Message reply = agent::chat_with_retry(hooks, log, chat, "test", token, 3, {});
    ASSERT_EQ(calls, 2);
    ASSERT_EQ(reply.content, "ok");
    ASSERT_EQ(reply.failure_status, 0L);
}

TEST(agent_turn_recovers_from_a_rate_limit) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    auto* raw = fake.get();
    agent_test::FakeReply limited;
    limited.error = "HTTP 429: rate limited";
    limited.status = 429;
    limited.retry_after_ms = 100;
    raw->script.push_back(std::move(limited));
    agent_test::FakeReply recovered;
    recovered.content = "recovered";
    raw->script.push_back(std::move(recovered));
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    const std::string out = ag.run("hello");
    ASSERT_EQ(raw->chat_calls, 2);
    ASSERT_EQ(out, "recovered");
}

// Even after the rate-limit retry budget, the turn continues: the model sees
// the harness notice and can keep working with wait/throttle. It never becomes
// a final answer that drops the session.
TEST(agent_turn_survives_exhausted_rate_limits) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    auto* raw = fake.get();
    for (int i = 0; i < 3; ++i) {
        agent_test::FakeReply limited;
        limited.error = "HTTP 429: rate limited";
        limited.status = 429;
        limited.retry_after_ms = 0;
        raw->script.push_back(std::move(limited));
    }
    agent_test::FakeReply finished;
    finished.content = "waited and finished";
    raw->script.push_back(std::move(finished));

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    const std::string out = ag.run("hello");
    ASSERT_EQ(raw->chat_calls, 4);
    ASSERT_EQ(out, "waited and finished");

    bool saw_notice = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness && m.content.find("rate limited") != std::string::npos)
            saw_notice = true;
    ASSERT(saw_notice);
}

TEST(agent_set_mode_publishes_the_status_snapshot) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.mode = agent::AgentMode::Write;
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.set_mode(agent::AgentMode::Yolo);
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT(snapshot->mode == agent::AgentMode::Yolo);

    ag.set_mode(agent::AgentMode::Read);
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT(snapshot->mode == agent::AgentMode::Read);
}

// A model switch is an automatic resolution: it applies the window the new
// model reports, and an unknown window clears the previous model's value.
// It never becomes a user pin.
TEST(agent_set_model_scopes_an_unpinned_window) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 131072;
    cfg.context_explicit = false;
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.set_model("other");
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 0);
    ASSERT_FALSE(snapshot->context_explicit);

    ag.set_model("big", caps(524288));
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 524288);
    ASSERT_FALSE(snapshot->context_explicit);
}

TEST(agent_set_model_carries_capabilities) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    agent::ModelCapabilities full = caps(1000);
    full.max_output = 8192;
    full.reasoning = true;
    full.supported_parameters = {"reasoning"};
    ag.set_model("m", full);
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->model_max_output, 8192);
    ASSERT(snapshot->model_supports_reasoning);
    ASSERT_EQ(snapshot->model_supported_parameters.size(), 1u);

    // A same-model call without capabilities keeps what the catalogue set.
    ag.set_model("m");
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->model_max_output, 8192);

    // A new model without capabilities is unknown, not the previous model's.
    ag.set_model("m2");
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->model_max_output, 0);
    ASSERT_FALSE(snapshot->model_supports_reasoning);
    ASSERT(snapshot->model_supported_parameters.empty());
}

// A user pin survives a model switch, a provider switch and a catalogue push.
TEST(agent_user_pin_survives_context_resolution) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 200000;
    cfg.context_explicit = true;
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.set_model("big", caps(524288));
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 200000);
    ASSERT_TRUE(snapshot->context_explicit);

    agent::Config resolved = cfg;
    resolved.provider_name = "other";
    resolved.context_size = 0;
    ag.set_provider(resolved, "big");
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 200000);

    ag.set_context_window(1);
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 200000);
}

// A provider switch adopts the resolved default (or 0 = unknown) instead of
// keeping the previous endpoint's window; the catalogue push uses the same
// method, without rebuilding the client.
TEST(agent_set_provider_and_context_window_adopt_the_resolution) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 131072;
    cfg.context_explicit = false;
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    agent::Config resolved = cfg;
    resolved.provider_name = "other";
    resolved.context_size = 524288;
    ag.set_provider(resolved, "big");
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 524288);
    ASSERT_FALSE(snapshot->context_explicit);

    ag.set_context_window(262144);
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 262144);
}

// A user pin is independent of the catalogue: it survives a model switch, and
// clearing it resolves the CURRENT model's known window (0 when unknown), not
// the pin it replaced.
TEST(agent_pin_and_clear_context_window) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 131072;
    cfg.context_explicit = false;
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.set_model("first", caps(65536));
    ag.pin_context_window(200000);
    auto snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 200000);
    ASSERT_TRUE(snapshot->context_explicit);

    ag.set_model("second", caps(524288));
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 200000);
    ASSERT_TRUE(snapshot->context_explicit);

    ag.clear_context_pin();
    snapshot = ag.config_snapshot();
    ASSERT(snapshot != nullptr);
    ASSERT_EQ(snapshot->context_size, 524288);
    ASSERT_FALSE(snapshot->context_explicit);
}

// A window learned from a 400 overflow rejection clamps the configured one:
// the runtime truth wins (a model's trained n_ctx can exceed the server's
// actual --ctx-size), so the gate fires at 70% of the LEARNED window.
TEST(agent_loop_learned_window_clamps_gate) {
    class LearnedFake : public agent_test::FakeLLMClient {
    public:
        int learned_context_size() const override { return 131072; }
    };
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 262144; // configured from model metadata (trained)
    cfg.compression_threshold = 0.7;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<LearnedFake>();
    agent_test::FakeLLMClient* raw = fake.get();
    // The first warm reply reports 95k prompt tokens: 72% of the LEARNED
    // 131072 window (fires on the next turn) but only 36% of the configured
    // 262144 (no fire without the clamp).
    {
        agent_test::FakeReply r;
        r.content = "warm reply";
        r.prompt_tokens = 95000;
        fake->script.push_back(std::move(r));
    }
    for (int i = 0; i < 20; ++i)
        push_text(*fake, "warm");
    {
        agent_test::FakeReply r;
        r.content = R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
                    R"("memories":[],"skills":[]})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "compressed after learned clamp");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i)
        ag.run("warm " + std::to_string(i));

    std::string reply = ag.run("next");
    ASSERT_EQ(reply, "compressed after learned clamp");
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("CLASSIFICATION of every older turn range") != std::string::npos)
                saw_compression = true;
    ASSERT(saw_compression);
}

// The pipeline must forward BOTH the classification segments and the
// memory/skill ops to the caller — previously only the ops survived, so the
// reported core/context/prune counts were always zero.
// The classify and extract calls must replay the live request prefix — the
// injected blocks and the tool schemas — or the server re-prefills the whole
// conversation, which is the cost the pipeline exists to avoid. The blocks are
// request-only: they never enter the compressed context.
// A compaction that cannot reduce the history must not replace it: the summary
// alone would cost tokens and change the prefix for nothing. A single-turn
// session protects its whole turn, so the honest outcome is "unchanged".
TEST(compression_does_not_grow_a_session_it_cannot_reduce) {
    agent::CompressionConfig cc;
    cc.context_size = 4096;
    cc.target_pct = 10;
    cc.keep_last_prompts = 10;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_text(*fake,
              R"({"summary":"work state","classification":[{"turns":"1-30","tag":"core"}]})");
    push_text(*fake, R"({"memories":[],"skills":[]})");

    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "one long turn";
    ctx.push(std::move(user));
    for (int r = 0; r < 10; ++r) {
        agent::Message call;
        call.role = "assistant";
        call.tool_calls = json::array(
            {{{"id", "c" + std::to_string(r)},
              {"type", "function"},
              {"function",
               {{"name", "read"}, {"arguments", "{\"path\":\"f" + std::to_string(r) + "\"}"}}}}});
        ctx.push(std::move(call));
        agent::Message result;
        result.role = "tool";
        result.tool_call_id = "c" + std::to_string(r);
        result.name = "read";
        result.content = std::string(100, 'x'); // too small to prune
        ctx.push(std::move(result));
    }
    const std::size_t before_tokens = ctx.token_count();

    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr);
    ASSERT(agent::estimate_tokens(out) <= before_tokens);
}

TEST(compression_request_replays_the_live_prefix) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(
        *fake,
        R"({"summary":"work state","classification":[{"turns":"1-1","tag":"core","summary":""}]})");
    push_text(*fake, R"({"memories":[],"skills":[]})");

    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "hello";
    ctx.push(std::move(user));

    agent::CompressionRequestPrefix prefix;
    agent::Message head;
    head.role = "system";
    head.content = "MEMORY-BLOCK";
    prefix.head_blocks.push_back(head);
    agent::Message tail;
    tail.role = "system";
    tail.content = "BRIEF-BLOCK";
    prefix.tail_blocks.push_back(tail);
    prefix.tools.push_back(std::make_shared<PrefixProbeTool>());

    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr, prefix);
    ASSERT_EQ(raw->requests.size(), size_t{2});
    const auto& classify_req = raw->requests[0];
    // [system][memory][conversation][brief][instruction]
    ASSERT_EQ(classify_req.size(), size_t{5});
    ASSERT_EQ(classify_req[0].content, std::string("Amber"));
    ASSERT_EQ(classify_req[1].content, std::string("MEMORY-BLOCK"));
    ASSERT_EQ(classify_req[2].content, std::string("hello"));
    ASSERT_EQ(classify_req[3].content, std::string("BRIEF-BLOCK"));
    ASSERT_EQ(raw->tool_counts[0], size_t{1});

    // The extract call replays the same prefix, then the classify pair.
    const auto& extract_req = raw->requests[1];
    for (size_t i = 0; i < classify_req.size(); ++i) {
        ASSERT_EQ(extract_req[i].role, classify_req[i].role);
        ASSERT_EQ(extract_req[i].content, classify_req[i].content);
    }
    ASSERT_EQ(raw->tool_counts[1], size_t{1});

    for (const auto& m : out)
        ASSERT(m.content.find("MEMORY-BLOCK") == std::string::npos);
}

TEST(compression_pipeline_forwards_segments_and_ops) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply classify;
    classify.content =
        R"({"classification":[{"turns":"0-1","tag":"context","summary":"greeting"}],)"
        R"("memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[{"name":"fact-1","content":"a fact","action":"upsert"}],)"
                      R"("skills":[]})";
    fake->script.push_back(std::move(extract));
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "hello";
    ctx.push(std::move(user));
    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr);
    ASSERT(!out.empty());
    ASSERT_EQ(cr.segments.size(), size_t{1});
    ASSERT(cr.segments[0].tag == agent::Classification::context);
    ASSERT_EQ(cr.memory_ops.size(), size_t{1});
    // The pipeline is pure — it reads the context and works on a copy, so
    // the live context is untouched by the classify/extract calls.
    ASSERT_EQ(ctx.size(), size_t{2});
    (void)ctx.get_all(); // hash chain intact
}

// KV-reuse guarantee: the compression pipeline makes TWO LLM calls
// (classify, then extract). For the server's prompt cache to stay warm the
// extract request must replay the classify request as a prefix — the second
// call then extends the first's KV instead of triggering a full prefill.
// This test pins that content-identity contract. It is the real mechanism
// behind the spec's "no full prefill between the compression calls" — the
// pipeline achieves it with a pure copy, not by mutating the live context.
TEST(compression_extract_request_shares_classify_prefix) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();

    agent_test::FakeReply classify;
    classify.content =
        R"({"classification":[{"turns":"0-1","tag":"context","summary":"greeting"}],)"
        R"("memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[],"skills":[]})";
    fake->script.push_back(std::move(extract));

    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "hello";
    ctx.push(std::move(user));

    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr);
    ASSERT(!out.empty());
    // Exactly two LLM calls: classify, then extract.
    ASSERT_EQ(raw->requests.size(), size_t{2});

    const auto& classify_req = raw->requests[0];
    const auto& extract_req = raw->requests[1];
    // The extract request is strictly longer and its leading messages are
    // byte-identical to the classify request (the classify prompt + a stored
    // "(classification consumed)" assistant reply are replayed before the
    // extract prompt).
    ASSERT(extract_req.size() > classify_req.size());
    for (size_t i = 0; i < classify_req.size(); ++i) {
        ASSERT_EQ(extract_req[i].role, classify_req[i].role);
        ASSERT_EQ(extract_req[i].content, classify_req[i].content);
    }
    // The classify prompt itself is present in both (the shared prefix).
    bool saw_classify = false;
    for (const auto& m : classify_req)
        if (m.content.find("CLASSIFICATION of every older turn range") != std::string::npos)
            saw_classify = true;
    ASSERT(saw_classify);
}

// Spec invariant 7: if any LLM call or parse fails, the input history is
// returned unchanged. The loop-collapse pass must not leak into the failure
// path — previously the caller rebuilt the context from the collapsed copy.
// Both compression requests report their own usage to the observer, tagged as
// Compression so the session ledger can attribute the spend.
TEST(compression_pipeline_reports_usage_per_request) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply classify;
    classify.content =
        R"({"classification":[{"turns":"0-1","tag":"context","summary":"greeting"}],)"
        R"("memories":[],"skills":[]})";
    classify.prompt_tokens = 900;
    classify.completion_tokens = 40;
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[],"skills":[]})";
    extract.prompt_tokens = 950;
    extract.completion_tokens = 12;
    fake->script.push_back(std::move(extract));
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "hello";
    ctx.push(std::move(user));

    struct UsageObserver : agent::CompressionObserver {
        std::vector<agent::Stats> samples;
        void on_llm_usage(const agent::Stats& stats) override { samples.push_back(stats); }
    } observer;
    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, &observer, &cr);
    ASSERT(!out.empty());
    ASSERT_EQ(observer.samples.size(), std::size_t{2});
    ASSERT_EQ(observer.samples[0].prompt_tokens, 900L);
    ASSERT_EQ(observer.samples[0].completion_tokens, 40L);
    ASSERT_EQ(observer.samples[1].prompt_tokens, 950L);
    ASSERT_EQ(observer.samples[1].completion_tokens, 12L);
}

TEST(compression_pipeline_failure_returns_history_unchanged) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply garbage;
    garbage.content = "this is not json";
    fake->script.push_back(std::move(garbage));
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message user;
    user.role = "user";
    user.content = "search for x";
    ctx.push(std::move(user));
    for (int i = 0; i < 3; ++i) {
        ctx.push(agent_test::tool_call_msg("search", json{{"pattern", "x"}}));
        agent::Message res;
        res.role = "tool";
        res.name = "search";
        res.content = "result";
        ctx.push(std::move(res));
    }
    auto before = ctx.get_all();
    auto out = compressor->compress(ctx, cc, *fake, nullptr, nullptr);
    // Identical size and the tool loop is intact (no collapse on failure).
    ASSERT_EQ(out.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i)
        ASSERT(out[i].content == before[i].content);
}

// CP-08: a failed EXTRACTION is non-fatal. The classification has already been
// applied, so the caller still gets the compressed history and the
// classification result — only the memory/skill ops are missing.
// Retention channels the harness owns: the classifier's summary, one archive
// entry for a pruned turn, and the active turn verbatim. The checkpoint text
// must survive a compaction that removes everything older than the tail.
TEST(compression_preserves_the_summary_archive_and_tail) {
    agent::CompressionConfig cc;
    cc.context_size = 8192;
    cc.target_pct = 10;
    cc.keep_last_prompts = 1;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_text(*fake, R"({"summary":"DECISION: the cache layer owns eviction","classification":[)"
                     R"({"turns":"1-2","tag":"context","summary":"explored the cache layer"},)"
                     R"({"turns":"3-4","tag":"core","summary":""}]})");
    push_text(*fake, R"({"memories":[],"skills":[]})");

    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message old_user;
    old_user.role = "user";
    old_user.content = "explore the cache";
    ctx.push(std::move(old_user));
    agent::Message old_reply;
    old_reply.role = "assistant";
    old_reply.content = std::string(4000, 'x'); // pruned: pays for the archive
    ctx.push(std::move(old_reply));
    agent::Message active;
    active.role = "user";
    active.content = "CHECKPOINT: keep wiring the eviction path";
    ctx.push(std::move(active));

    agent::CompressionResponse cr;
    const size_t before_tokens = ctx.token_count();
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr);
    bool summary = false;
    bool archive = false;
    bool tail = false;
    for (const auto& m : out) {
        if (m.content.find("DECISION: the cache layer owns eviction") != std::string::npos)
            summary = true;
        if (m.content.find("explored the cache layer") != std::string::npos)
            archive = true;
        if (m.content.find("CHECKPOINT: keep wiring the eviction path") != std::string::npos)
            tail = true;
    }
    ASSERT(summary);
    ASSERT(archive);
    ASSERT(tail);
    ASSERT(agent::estimate_tokens(out) < before_tokens);
}

TEST(compression_extract_failure_keeps_classification) {
    agent::CompressionConfig cc;
    cc.keep_last_prompts = 1; // the older turn is archivable, so the pass reduces
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();

    // First call (classify): prune the older (large) turn, keep the active one.
    agent_test::FakeReply classify;
    classify.content = R"({"classification":[{"turns":"1-2","tag":"prune","summary":""},)"
                       R"({"turns":"3-3","tag":"core","summary":""}]})";
    fake->script.push_back(std::move(classify));
    // Second call (extract): fail.
    agent_test::FakeReply boom;
    boom.error = "extract exploded";
    fake->script.push_back(std::move(boom));

    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    ctx.push(std::move(sys));
    agent::Message old_user;
    old_user.role = "user";
    old_user.content = "old task";
    ctx.push(std::move(old_user));
    agent::Message old_reply;
    old_reply.role = "assistant";
    old_reply.content = std::string(4000, 'x'); // big enough that dropping it pays
    ctx.push(std::move(old_reply));
    agent::Message user;
    user.role = "user";
    user.content = "search for x";
    ctx.push(std::move(user));

    agent::CompressionResponse cr;
    auto out = compressor->compress(ctx, cc, *fake, nullptr, &cr);

    // Classification succeeded, so there is no error and the segments are there.
    ASSERT_TRUE(cr.error.empty());
    ASSERT_FALSE(cr.segments.empty());
    // The extraction failed, so no ops were merged in.
    ASSERT_TRUE(cr.memory_ops.empty());
    ASSERT_TRUE(cr.skill_ops.empty());
    ASSERT_FALSE(cr.brief.has_value());
    // And the compressed history — not the original — is what the caller gets.
    bool has_compressed_context = false;
    for (const auto& m : out)
        if (m.content.rfind(agent::kCompressedContextPrefix, 0) == 0)
            has_compressed_context = true;
    ASSERT_TRUE(has_compressed_context);
}

// A failed compression must report the REAL error (with the before-stats) —
// not a zeroed CompressionResult that the TUI misreads as "no compressor
// configured". Regression: run_compression returned before setting
// messages_before, so a pipeline LLM failure looked identical to a missing
// compressor.
TEST(compression_failure_reports_real_error_not_no_compressor) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 32768;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    // Classify call fails (e.g. server error on the compression LLM call).
    agent_test::FakeReply err;
    err.error = "simulated server failure";
    fake->script.push_back(std::move(err));
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "Amber";
    msgs.push_back(std::move(sys));
    for (int i = 0; i < 6; ++i) {
        agent::Message u;
        u.role = "user";
        u.content = "q" + std::to_string(i);
        msgs.push_back(std::move(u));
        agent::Message m;
        m.role = "assistant";
        m.content = "a" + std::to_string(i);
        msgs.push_back(std::move(m));
    }
    ag.set_context(std::move(msgs));
    auto r = ag.compress_now();
    ASSERT(!r.error.empty());
    ASSERT(r.error.find("simulated server failure") != std::string::npos);
    // messages_before must reflect the real context size — not 0 (which the
    // TUI maps to "no compressor configured").
    ASSERT(r.messages_before > 0u);
    // Context untouched (spec invariant 7).
    ASSERT(ag.context().size() >= 13u);
}

// Aggressive compression: the pipeline must reduce a large context toward the
// configured target_pct of the window (default 10%), not stop at ~50% because
// the classifier+guard kept too much. A 262k window with a ~200k-token context
// and target_pct=10 must compress to ~26k estimated tokens.
TEST(compression_target_budget_enforced) {
    agent::test::XdgGuard xdg("compression_target_budget");
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 262144;
    agent::ToolRegistry reg;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    // Classifier: mark the OLD region context (archive) and recent as core,
    // but the guard + classifier would still keep a large tail; the budget
    // pass must archive down to ~10% regardless.
    agent_test::FakeReply classify;
    classify.content = R"({"summary":"Working on feature X.","classification":)"
                       R"([{"turns":"1-50","tag":"context","summary":"old work"},)"
                       R"({"turns":"51-80","tag":"core","summary":""}],"memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[],"skills":[]})";
    fake->script.push_back(std::move(extract));

    agent::Agent ag(cfg, reg, {}, std::move(compressor), {}, {}, {}, std::move(fake));
    // Seed a large context (~200k estimated tokens).
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "You are Amber. " + std::string(4000, 's');
    msgs.push_back(std::move(sys));
    for (int i = 0; i < 60; ++i) {
        agent::Message u;
        u.role = "user";
        u.content = "prompt " + std::to_string(i) + " " + std::string(2000, 'u');
        msgs.push_back(std::move(u));
        agent::Message a;
        a.role = "assistant";
        a.content = "answer " + std::to_string(i) + " " + std::string(2000, 'a');
        msgs.push_back(std::move(a));
        agent::Message t;
        t.role = "tool";
        t.name = "bash";
        t.content = "huge tool output " + std::string(4000, 'x');
        msgs.push_back(std::move(t));
    }
    ag.set_context(std::move(msgs));
    size_t before_tokens = ag.context().token_count();
    ASSERT(before_tokens > 100000u); // genuinely large

    auto r = ag.compress_now();
    ASSERT(r.error.empty());
    ASSERT(r.messages_after < r.messages_before);
    size_t after_tokens = ag.context().token_count();
    // ~10% of 262144 ≈ 26k; allow slack for the protected tail.
    ASSERT(after_tokens < 60000u);
    (void)r;
}

// The protected tail is configurable: keep_last_prompts user messages must
// survive verbatim, and their bulky tool outputs are still pruned (the largest
// token class is not exempt just because it is recent).
TEST(compression_keep_last_prompts_and_prune_tail) {
    agent::test::XdgGuard xdg("compression_prune_tail");
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 32768;
    cfg.compression_keep_last_prompts = 2;
    cfg.compression_keep_last_prompts_explicit = true;
    agent::ToolRegistry reg;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply classify;
    classify.content =
        R"({"summary":"s","classification":)"
        R"([{"turns":"0-40","tag":"context","summary":"old"}],"memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[],"skills":[]})";
    fake->script.push_back(std::move(extract));

    agent::Agent ag(cfg, reg, {}, std::move(compressor), {}, {}, {}, std::move(fake));
    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "You are Amber.";
    msgs.push_back(std::move(sys));
    // 12 user turns; last 2 carry huge tool outputs that must be pruned.
    for (int i = 0; i < 12; ++i) {
        agent::Message u;
        u.role = "user";
        u.content = "prompt " + std::to_string(i);
        msgs.push_back(std::move(u));
        agent::Message a;
        a.role = "assistant";
        a.content = "answer " + std::to_string(i);
        msgs.push_back(std::move(a));
        agent::Message t;
        t.role = "tool";
        t.name = "bash";
        t.content = std::string(5000, 'z'); // bulky — should be pruned even in tail
        msgs.push_back(std::move(t));
    }
    ag.set_context(std::move(msgs));

    auto r = ag.compress_now();
    ASSERT(r.error.empty());
    // After compression, the last two user prompts must still be present.
    auto ctx = ag.context().get_all();
    int last_user_seen = -1;
    for (size_t i = 0; i < ctx.size(); ++i)
        if (ctx[i].role == "user")
            last_user_seen = static_cast<int>(i);
    // The final compressed context ends with the last user prompt's turn.
    bool found_last = false;
    for (const auto& m : ctx)
        if (m.role == "user" && m.content == "prompt 11")
            found_last = true;
    ASSERT(found_last);
    // Bulky tool outputs anywhere (incl. the recent tail) are placeholder'd.
    bool saw_huge = false;
    for (const auto& m : ctx)
        if (m.role == "tool" && m.content.size() > 1000)
            saw_huge = true;
    ASSERT(!saw_huge);
    (void)last_user_seen;
}

// Reasoning/thinking text dominates thinking-model sessions (never sent back
// to the server) — compression must strip it, and internal confirmation probes
// ("Are you finished?") must not count as real user prompts in the guard.
TEST(compression_strips_reasoning_and_ignores_probes) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 262144;
    agent::ToolRegistry reg;
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply classify;
    classify.content =
        R"({"summary":"s","classification":[{"turns":"0-60","tag":"context","summary":"old"}],"memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content = R"({"memories":[],"skills":[]})";
    fake->script.push_back(std::move(extract));
    agent::Agent ag(cfg, reg, {}, std::move(compressor), {}, {}, {}, std::move(fake));

    std::vector<agent::Message> msgs;
    agent::Message sys;
    sys.role = "system";
    sys.content = "You are Amber.";
    msgs.push_back(std::move(sys));
    // 6 REAL prompts, each followed by a confirmation probe + a bulky
    // reasoning assistant turn. Fewer real prompts than keep_last_prompts(10)
    // would protect everything if probes counted as users.
    for (int i = 0; i < 6; ++i) {
        agent::Message u;
        u.role = "user";
        u.content = "real prompt " + std::to_string(i);
        msgs.push_back(std::move(u));
        agent::Message probe;
        probe.role = "user";
        probe.content =
            "Are you finished? If you need more information or analysis, use tools now.";
        msgs.push_back(std::move(probe));
        agent::Message a;
        a.role = "assistant";
        a.content = "answer " + std::to_string(i);
        a.reasoning = std::string(9000, 'r'); // thinking bloat
        msgs.push_back(std::move(a));
    }
    ag.set_context(std::move(msgs));
    size_t before = ag.context().token_count();
    ASSERT(before > 10000u); // reasoning inflates the count

    auto r = ag.compress_now();
    ASSERT(r.error.empty());
    auto ctx = ag.context().get_all();
    // No reasoning survives compression.
    for (const auto& m : ctx)
        if (m.role == "assistant")
            ASSERT(m.reasoning.empty());
    // The last real prompt survives (probes did not push it out of the guard).
    bool found_last = false;
    for (const auto& m : ctx)
        if (m.role == "user" && m.content == "real prompt 5")
            found_last = true;
    ASSERT(found_last);
    // Dramatically smaller.
    size_t after = ag.context().token_count();
    ASSERT(after < before / 3);
}

// [AL-12] The context hash chain survives a full session (get_all() asserts
// integrity on every read; every test above already exercises it).
TEST(agent_loop_hash_chain_intact) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "done reading");
    push_text(*fake, "second turn");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.run("read the Makefile");
    // Multiple reads over the live stack; each get_all() verifies the chain.
    (void)ag.context().get_all();
    ag.run("and again");
    (void)ag.context().get_all();
    ASSERT(raw->chat_calls >= 3);
}

// ---------------------------------------------------------------------------
// Retry policy ([AL-07]..[AL-10]): the fake throws retryable/non-retryable
// ApiErrors; chat_with_retry applies backoff, then the loop degrades.
// ---------------------------------------------------------------------------

// [AL-07] Two transient failures, then success.
TEST(agent_loop_retry_then_success) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    agent_test::FakeReply f1;
    f1.error = "server hiccup";
    f1.retryable = true;
    fake->script.push_back(std::move(f1));
    agent_test::FakeReply f2;
    f2.error = "server hiccup again";
    f2.retryable = true;
    fake->script.push_back(std::move(f2));
    push_text(*fake, "ok after retries");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "ok after retries");
    ASSERT_EQ(raw->chat_calls, 3); // 2 failed attempts + the successful one
}

// [AL-08] A non-retryable error fails fast (single attempt) and reaches the
// model as a [harness] notice, so the next reply can react to it.
TEST(agent_loop_non_retryable_fails_fast) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    agent_test::FakeReply bad;
    bad.error = "invalid api key";
    bad.retryable = false;
    fake->script.push_back(std::move(bad));
    push_text(*fake, "I cannot reach the server with the current key.");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "I cannot reach the server with the current key.");
    ASSERT_EQ(raw->chat_calls, 2); // 1 attempt + the model's answer to the notice
    bool noticed = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness) {
            ASSERT_EQ(m.role, "user");
            ASSERT(m.content.find("invalid api key") != std::string::npos);
            ASSERT(m.content.find("1 attempt") != std::string::npos);
            noticed = true;
        }
    ASSERT(noticed);
}

// [AL-09] Retries exhausted -> one [harness] notice naming the attempts, then
// the model's own reply; the conversation stays intact and the prompt is kept
// for a manual retry.
TEST(agent_loop_retries_exhausted) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 3; ++i) {
        agent_test::FakeReply f;
        f.error = "server down";
        f.retryable = true;
        fake->script.push_back(std::move(f));
    }
    push_text(*fake, "the server is down; nothing ran.");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "the server is down; nothing ran.");
    ASSERT_EQ(raw->chat_calls, 4); // 3 attempts + the model's answer
    const auto& ctx = ag.context().get_all();
    bool saw_user = false;
    bool saw_notice = false;
    for (const auto& m : ctx) {
        if (m.role == "user" && m.content == "hi")
            saw_user = true;
        if (m.harness)
            saw_notice = true;
        // No fabricated text in the model's voice.
        if (m.role == "assistant")
            ASSERT(m.content.find("[harness]") == std::string::npos);
    }
    ASSERT(saw_user);
    ASSERT(saw_notice);
    bool notice_counts = false;
    for (const auto& m : ctx)
        if (m.harness && m.content.find("3 attempts") != std::string::npos)
            notice_counts = true;
    ASSERT(notice_counts);
}

// [AL-17] Three failed requests in a row stop the turn: the endpoint, not the
// model, is the problem, and the record says so.
TEST(agent_loop_repeated_failures_stop_the_turn) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 9; ++i) {
        agent_test::FakeReply f;
        f.error = "HTTP 503 from LLM server: upstream_error";
        f.retryable = true;
        fake->script.push_back(std::move(f));
    }
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT(reply.find("agent stopped") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 9); // three requests, three attempts each
    int failures = 0;
    int stops = 0;
    for (const auto& m : ag.context().get_all()) {
        if (!m.harness)
            continue;
        // The stop note names the last failure too; it is the stop, not a fourth
        // request-failed notice.
        if (m.content.find("ended without a closing reply") != std::string::npos) {
            ++stops;
            continue;
        }
        if (m.content.find("request failed") != std::string::npos)
            ++failures;
    }
    ASSERT_EQ(failures, 3); // one per exhausted request, then the bound
    ASSERT_EQ(stops, 1);    // the stop itself is part of the record
}

// The retry ladder grows with the attempt and is jittered per failure: an
// endpoint that is recovering (or throttling) is not met with a metronome, and
// a given failure still retries the same way every time.
TEST(retry_backoff_grows_with_attempts_and_spreads_by_failure) {
    const std::string seed = "HTTP 503 from LLM server: upstream_error";
    const long a1 = agent::retry_backoff_ms(1, seed);
    const long a3 = agent::retry_backoff_ms(3, seed);
    const long a9 = agent::retry_backoff_ms(9, seed);
    ASSERT(a1 >= 750 && a1 <= 1250);
    ASSERT(a3 >= 3000 && a3 <= 5000);
    ASSERT(a9 >= 6000 && a9 <= 10000); // capped at 8s
    ASSERT_EQ(agent::retry_backoff_ms(3, seed), a3);
    // Distinct failures do not line up on the same instant.
    const long b1 = agent::retry_backoff_ms(1, "HTTP 500 from LLM server: boom");
    const long c1 = agent::retry_backoff_ms(1, "connection reset by peer");
    ASSERT(a1 != b1 || b1 != c1);
}

// [AL-20] What the client had to do to a request reaches the model: a window
// the server clamped, a call it had to drop. Hidden state about the model's own
// request is how it ends up re-issuing work that never had a chance.
TEST(agent_loop_client_notices_reach_the_model) {
    class NoticingFake : public agent_test::FakeLLMClient {
    public:
        std::vector<std::string> take_notices() override {
            std::vector<std::string> out = std::move(notices_);
            notices_.clear();
            return out;
        }
        std::vector<std::string> notices_{"the endpoint clamped the window to 8192 tokens"};
    };
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<NoticingFake>();
    NoticingFake* raw = fake.get();
    push_text(*fake, "noted");
    push_text(*fake, "second turn");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ASSERT_EQ(ag.run("first"), "noted");
    bool in_context = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness && m.content.find("clamped the window") != std::string::npos)
            in_context = true;
    ASSERT(in_context);
    // The next request carries it, in place, as the environment speaking.
    ASSERT_EQ(ag.run("second"), "second turn");
    ASSERT(raw->requests.size() >= 2u);
    bool in_request = false;
    for (const auto& m : raw->requests[1])
        if (m.harness && m.content.find("clamped the window") != std::string::npos)
            in_request = true;
    ASSERT(in_request);
}

// [AL-18] The loop's own state reaches the model before a bound bites: a ceiling
// that arrives without warning is a trap, so the crossing is announced once.
TEST(agent_loop_budget_note_precedes_the_cap) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.max_tool_iterations = 5;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    const char* paths[] = {"Makefile", "Makefile.in", "lib/llm.cpp", "lib/agent.cpp", "README.md"};
    for (const char* path : paths)
        push_tool_call(*fake, "read", {{"path", path}});
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.run("read everything");
    // The notice rides the request the model actually sees, and it arrives
    // before the cap does not: nothing about step 1-3 mentioned the budget.
    ASSERT(raw->requests.size() == 5u);
    for (size_t i = 0; i < 3; ++i)
        for (const auto& m : raw->requests[i])
            ASSERT(m.content.find("step 4/5") == std::string::npos);
    bool announced = false;
    for (const auto& m : raw->requests[3])
        if (m.harness && m.content.find("step 4/5") != std::string::npos)
            announced = true;
    ASSERT(announced);
}

// [AL-19] A repeated call is reported, never acted on: the harness bounds and
// says so, the model decides. The escalation argument (bash `timeout`) is not a
// change, which is what made the old guard blind to 30 retries.
TEST(agent_loop_repeat_is_reported_not_reenforced) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 3; ++i)
        push_tool_call(*fake, "read", {{"path", "Makefile"}}, "c" + std::to_string(i));
    push_text(*fake, "I read the same file three times; closing.");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    const std::string reply = ag.run("read the Makefile");
    ASSERT_EQ(reply, "I read the same file three times; closing.");
    ASSERT_EQ(raw->chat_calls, 4); // three tool turns, then the close — nothing stopped it
    bool noted = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness && m.content.find("has been issued") != std::string::npos)
            noted = true;
    ASSERT(noted);
}

// The canonical call key ignores the bash escalation argument, so "the same
// command with a longer timeout" is the same command.
TEST(canonical_call_key_ignores_timing_arguments) {
    const json slow = json::array(
        {{{"id", "c1"},
          {"type", "function"},
          {"function",
           {{"name", "bash"}, {"arguments", R"({"command":"make test","timeout":30})"}}}}});
    const json slower = json::array(
        {{{"id", "c2"},
          {"type", "function"},
          {"function",
           {{"name", "bash"}, {"arguments", R"({"command":"make test","timeout":600})"}}}}});
    const json other = json::array(
        {{{"id", "c3"},
          {"type", "function"},
          {"function",
           {{"name", "bash"}, {"arguments", R"({"command":"make lint","timeout":30})"}}}}});
    ASSERT_EQ(agent::canonical_call_key(slow), agent::canonical_call_key(slower));
    ASSERT(agent::canonical_call_key(slow) != agent::canonical_call_key(other));
}

// A closing marker is just text now: the whole reply is the answer, and the
// harness generates nothing after it. Every miss used to re-inject
// "Are you finished?" and buy another generation, which is how a finished
// coverage review became 98 consecutive bash calls.
TEST(agent_loop_done_marker_is_part_of_the_answer) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "summary of findings. **done.**");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ASSERT_EQ(ag.run("hi"), "summary of findings. **done.**");
    ASSERT_EQ(raw->chat_calls, 1);
}

// A transient outage that the retry loop survives is invisible per attempt —
// the model is told nothing it cannot act on. What it must never miss is the
// exhausted case: covered by [AL-09].
TEST(agent_loop_recovered_outage_is_silent) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 2; ++i) {
        agent_test::FakeReply f;
        f.error = "HTTP 503 from LLM server: {\"error_type\":\"upstream_error\"}";
        f.retryable = true;
        fake->script.push_back(std::move(f));
    }
    push_text(*fake, "answer after the hiccup");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    const std::string reply = ag.run("hi"); // must not throw
    ASSERT_EQ(reply, "answer after the hiccup");
    ASSERT_EQ(raw->chat_calls, 3); // 2 failed attempts + the successful one
    for (const auto& m : ag.context().get_all())
        ASSERT(!m.harness); // nothing to report: the turn never noticed
}

// [AL-10] Cancellation during the backoff aborts the wait.
TEST(agent_loop_cancel_during_backoff) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    agent_test::FakeReply f;
    f.error = "server down";
    f.retryable = true;
    fake->script.push_back(std::move(f));

    std::thread canceller([&cfg]() {
        usleep(150 * 1000);
        cfg.cancel_token.request();
    });
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    auto t0 = std::chrono::steady_clock::now();
    std::string reply = ag.run("hi");
    canceller.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    // Cancellation stops the turn cleanly: no fabricated message, one aborted
    // generation attempt, empty reply.
    ASSERT(reply.empty());
    ASSERT_EQ(raw->chat_calls, 1);
    ASSERT(elapsed.count() < 900);
}

// [AL-11] Unknown n_ctx: the gate falls back to a conservative budget instead
// An unknown context window disables AUTO-compression (no guessed budget —
// an arbitrary fallback fired at a tiny fraction of modern windows). Manual
// /compress and the 400-overflow learner still cover it.
TEST(agent_loop_unknown_context_never_auto_fires) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 0; // server never reported n_ctx
    cfg.compression_threshold = 0.1;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 5; ++i) {
        push_text(*fake, "warm");
    }
    push_text(*fake, "direct reply");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));
    for (int i = 0; i < 5; ++i)
        ag.run("warm " + std::to_string(i));

    std::string big_prompt(8000, 'x');
    std::string reply = ag.run(big_prompt);
    ASSERT_EQ(reply, "direct reply");
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("CLASSIFICATION of every older turn range") != std::string::npos)
                saw_compression = true;
    ASSERT_FALSE(saw_compression);
    (void)ag.context().get_all();
}

// [FIX-015 residual] /set model must reach the RUNNING agent. The LLM client
// holds a config snapshot (HttpLLMClient copies cfg at construction), so a
// model switch has to rebuild the client — otherwise the conversation keeps
// talking to the old model while the config file says otherwise.
TEST(agent_set_model_swaps_client) {
    agent::Config cfg = loop_cfg();
    cfg.model = "old-model";
    agent::ToolRegistry reg;
    std::vector<std::string> seen_models;
    auto factory = [&](const agent::Config& c) {
        seen_models.push_back(c.model);
        return std::make_unique<agent_test::FakeLLMClient>();
    };
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, {}, factory);
    ASSERT_EQ(seen_models.size(), 1u);
    ASSERT_EQ(seen_models[0], "old-model");
    ag.set_model("ornith-35b");
    ASSERT_EQ(seen_models.size(), 2u);
    ASSERT_EQ(seen_models[1], "ornith-35b");
}

// [GR] A template-parser 400 (the server cannot parse the tool grammar for
// the loaded model) must not kill the turn: the request is retried once
// WITHOUT tools and the conversation continues on the same agent.
TEST(agent_template_parser_400_recovers_without_tools) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    agent_test::FakeReply fail;
    fail.error = "HTTP 400 from LLM server: Unable to generate parser for "
                 "this template";
    fail.retryable = false;
    fake->script.push_back(std::move(fail));
    push_text(*fake, "hello there");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "hello there");
    // Generation attempt 1 carried tools; the adapted retry dropped them.
    ASSERT_EQ(raw->tool_counts.size(), 2u);
    ASSERT(raw->tool_counts[0] > 0u);   // first attempt carried tools
    ASSERT_EQ(raw->tool_counts[1], 0u); // adapted retry dropped them
}

// [GR] When even the adapted retry fails, the failure reaches the model as a
// [harness] notice with the attempt count rather than killing the turn.
TEST(agent_unrecoverable_request_reports_instead_of_throwing) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 4; ++i) {
        agent_test::FakeReply fail;
        fail.error = "HTTP 400 from LLM server: Unable to generate parser "
                     "for this template";
        fail.retryable = false;
        fake->script.push_back(std::move(fail));
    }
    push_text(*fake, "the template cannot parse tool calls here.");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    const std::string reply = ag.run("hi"); // must not throw
    ASSERT_EQ(reply, "the template cannot parse tool calls here.");
    ASSERT_EQ(raw->chat_calls, 5); // two exhausted requests, then the answer
    int repairs = 0;
    int failures = 0;
    for (const auto& m : ag.context().get_all()) {
        if (!m.harness)
            continue;
        if (m.content.find("retried without tools") != std::string::npos)
            ++repairs;
        if (m.content.find("Unable to generate parser") != std::string::npos)
            ++failures;
    }
    // Each request: the repair is announced, and when the repaired attempt also
    // fails the exhaustion is announced with the error and the attempt count.
    ASSERT_EQ(repairs, 2);
    ASSERT_EQ(failures, 2);
}

// [TC] Ornith attribute-style XML tool calls inside the reply content are
// extracted and executed — the regression from the session where
// <tool_call><function=bash><parameter=command> was emitted, never ran, and
// the model repeated it.
TEST(agent_loop_attribute_xml_tool_call_executes) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply r;
    r.content = "Let me check.\n"
                "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n"
                "</parameter>\n</function>\n</tool_call>";
    fake->script.push_back(std::move(r));
    push_text(*fake, "done reading");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("read the Makefile");
    ASSERT_EQ(reply, "done reading");
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" && m.content.find("Makefile") != std::string::npos)
            saw_tool_result = true;
    ASSERT(saw_tool_result);
}

// [GR] XML tool calls fire on_tool_call exactly once per dispatched call.
// The extraction path used to fire the hook AND dispatch fired it again,
// producing two "running" lines in the TUI (the fold replace only collapsed
// the last one).
TEST(agent_xml_tool_call_fires_hook_once) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply r;
    r.content = "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n"
                "</parameter>\n</function>\n</tool_call>";
    fake->script.push_back(std::move(r));
    push_text(*fake, "done reading");
    int tool_call_hook_count = 0;
    agent::AgentHooks hooks;
    hooks.on_tool_call = [&](const std::string&, const agent::json&) { ++tool_call_hook_count; };
    agent::Agent ag(cfg, reg, hooks, {}, {}, {}, {}, std::move(fake));
    ag.run("read the Makefile");
    ASSERT_EQ(tool_call_hook_count, 1);
}

// [P1] The todowrite tool's host-owned store persists across turns (the
// model replaces the full list each call; state survives in TodoStore, not
// in context).
TEST(agent_loop_todowrite_state_persists) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{});
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(
        *fake, "todowrite",
        {{"todos", {{{"id", "p1"}, {"text", "fix parsing"}, {"status", "in_progress"}}}}});
    push_tool_call(*fake, "todowrite",
                   {{"todos",
                     {{{"id", "p1"}, {"text", "fix parsing"}, {"status", "completed"}},
                      {{"id", "p2"}, {"text", "write tests"}, {"status", "pending"}}}}});
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("track the work");
    ASSERT_EQ(reply, "done");
    // The final full-list replacement is what the store holds.
    ASSERT_EQ(todos.items().size(), 2u);
    ASSERT(todos.items()[0].status == agent::TodoStatus::Completed);
    ASSERT_EQ(todos.items()[1].text, "write tests");
    ASSERT(raw->chat_calls >= 3);
}

// The governor's model-facing controls: status/set/reset on the active
// provider, and a bounded one-off wait. They are tools, not hardcoded prompt
// incantations, so the model discovers them with the rest of the set.
TEST(rate_tools_are_registered_and_bounded) {
    agent::Workspace::set_root(cwd());
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{});
    const auto wait_tool = reg.find("wait");
    const auto throttle_tool = reg.find("throttle");
    ASSERT(wait_tool != nullptr);
    ASSERT(throttle_tool != nullptr);

    agent::Config cfg;
    cfg.provider_name = "rate-tool-test";
    cfg.api_base = "https://rate-tool.example/v1";
    cfg.api_key = "rate-tool-key";
    agent::RunScope leaf;
    leaf.config = &cfg;
    agent::ScopedRunScope scope(leaf);

    const agent::ToolResult too_long = wait_tool->execute({{"seconds", 301}});
    ASSERT_FALSE(too_long.ok);
    const agent::ToolResult zero = wait_tool->execute({{"seconds", 0}});
    ASSERT_TRUE(zero.ok);

    const agent::ToolResult status = throttle_tool->execute({{"action", "status"}});
    ASSERT_TRUE(status.ok);
    ASSERT(status.output.find("provider pacing") != std::string::npos);

    const agent::ToolResult set = throttle_tool->execute(
        {{"action", "set"}, {"interval_ms", 1000}, {"concurrency", 1}, {"ttl_seconds", 5}});
    ASSERT_TRUE(set.ok);
    ASSERT(set.output.find("interval=1000ms") != std::string::npos);
    ASSERT(set.output.find("concurrency=1") != std::string::npos);

    const agent::ToolResult reset = throttle_tool->execute({{"action", "reset"}});
    ASSERT_TRUE(reset.ok);
    ASSERT(reset.output.find("manual") == std::string::npos);
}

// [P2] The result envelope must not re-echo the full args JSON (write calls
// echoed whole edit payloads — context bloat on every result drives
// re-reading). status + meta stay.
TEST(agent_loop_tool_envelope_lean) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_write_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent::json edits = agent::json::array(
        {{{"old", ""}, {"new", std::string(300, 'L')}}}); // > 120 chars -> args must not echo
    push_tool_call(*fake, "write", {{"path", "bench_envelope_test.txt"}, {"edits", edits}});
    push_text(*fake, "done writing");
    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(fake));

    ag.run("create bench_envelope_test.txt");
    std::remove("bench_envelope_test.txt");
    const auto& ctx = ag.context().get_all();
    bool saw_lean = false;
    for (const auto& m : ctx) {
        if (m.role != "tool")
            continue;
        ASSERT(m.content.find("[tool=write status=ok") != std::string::npos);
        ASSERT(m.content.find("args=") == std::string::npos);
        ASSERT(m.content.find("[end]") != std::string::npos);
        saw_lean = true;
    }
    ASSERT(saw_lean);
}

// [P2v2] The envelope echoes args only when small — confirmation for the
// common case (read path=...), silence for large payloads (write edits).
TEST(agent_loop_tool_envelope_small_args_echoed) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "done reading");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.run("read the Makefile");
    const auto& ctx = ag.context().get_all();
    bool saw_echo = false;
    for (const auto& m : ctx) {
        if (m.role != "tool")
            continue;
        ASSERT(m.content.find("[tool=read args={\"path\":\"Makefile\"}") != std::string::npos);
        saw_echo = true;
    }
    ASSERT(saw_echo);
}

// ---------------------------------------------------------------------------
// [P4] Sub-agents (task tool) — focused worker with its own context
// ---------------------------------------------------------------------------

// Fake client sharing one script deque across parent and sub-agent
// instances (the sub-agent is constructed by the executor's factory).
struct FakeTrack {
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
};

class SharedScriptFake : public agent::LLMClient {
public:
    std::shared_ptr<std::deque<agent_test::FakeReply>> script =
        std::make_shared<std::deque<agent_test::FakeReply>>();
    std::shared_ptr<FakeTrack> track;
    std::vector<size_t> tool_counts;

    agent::ServerInfo probe_server() const override {
        agent::ServerInfo i;
        i.ok = true;
        i.model = "fake";
        return i;
    }

    // Reported, not stored: the client is destroyed with the sub-agent that
    // owns it, so a test that kept a pointer here would read freed memory.
    std::function<void(const std::string&)> on_system;

    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<std::shared_ptr<agent::Tool>>& tools,
                        agent::Stats* stats = nullptr) override {
        tool_counts.push_back(tools.size());
        if (on_system && !messages.empty())
            on_system(messages.front().content);
        if (track) {
            const int a = ++track->active;
            int p = track->peak.load();
            while (a > p && !track->peak.compare_exchange_weak(p, a)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            --track->active;
        }
        agent_test::FakeReply r;
        if (!script->empty()) {
            r = std::move(script->front());
            script->pop_front();
        }
        if (stats) {
            stats->prompt_tokens = r.prompt_tokens;
            stats->completion_tokens = r.completion_tokens;
            stats->valid = true;
        }
        if (!r.error.empty())
            throw agent::ApiError(r.retryable ? 503 : 401, r.retryable, r.error);
        agent::Message m;
        m.role = "assistant";
        m.content = r.content;
        m.tool_calls = r.tool_calls;
        return m;
    }

    agent::Message chat_stream(const std::vector<agent::Message>&,
                               const std::vector<std::shared_ptr<agent::Tool>>&,
                               const std::function<void(const agent::StreamChunk&)>&,
                               agent::Stats* stats = nullptr) override {
        return chat({}, {}, stats);
    }
};

// An HTTP 401 with an on_api_key hook: the one-shot auth repair prompts for a
// key, rebuilds the client with it (factory observes the new key), and retries
// the SAME turn — the reply comes back instead of degrading.
TEST(agent_loop_auth_401_recovers_with_new_key) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.provider_name = "kilocode";
    agent::ToolRegistry reg;
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    agent_test::FakeReply auth_err;
    auth_err.error = "HTTP 401 from LLM server: invalid api key";
    auth_err.retryable = false;
    script->push_back(std::move(auth_err));
    push_text(script, "recovered after key");

    std::vector<std::string> seen_keys;
    auto factory = [&](const agent::Config& c) {
        seen_keys.push_back(c.api_key);
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    };
    agent::AgentHooks hooks;
    hooks.on_api_key = [](const std::string&) { return std::string("sk-new"); };
    agent::Agent ag(cfg, reg, hooks, {}, {}, {}, {}, {}, factory);

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "recovered after key");
    // Client rebuilt: first construction (empty key) + set_connection (new key).
    ASSERT_EQ(seen_keys.size(), 2u);
    ASSERT_EQ(seen_keys[0], "");
    ASSERT_EQ(seen_keys[1], "sk-new");
}

// An HTTP 401 with the user declining the key prompt (hook returns ""): the
// turn degrades exactly like any other non-retryable failure — one attempt,
// then a [harness] notice the model can act on. No retry.
TEST(agent_loop_auth_401_declined_key_degrades) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    agent_test::FakeReply auth_err;
    auth_err.error = "HTTP 401 from LLM server: invalid api key";
    auth_err.retryable = false;
    script->push_back(std::move(auth_err));
    push_text(script, "the key was rejected; a new one is needed.");
    bool prompted = false;

    auto factory = [&](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    };
    agent::AgentHooks hooks;
    hooks.on_api_key = [&prompted](const std::string&) {
        prompted = true;
        return std::string(); // user cancelled
    };
    agent::Agent ag(cfg, reg, hooks, {}, {}, {}, {}, {}, factory);

    std::string reply = ag.run("hi");
    ASSERT(prompted);
    ASSERT_EQ(reply, "the key was rejected; a new one is needed.");
    bool notice = false;
    for (const auto& m : ag.context().get_all())
        if (m.harness && m.content.find("1 attempt") != std::string::npos)
            notice = true;
    ASSERT(notice);
}

// The parent delegates one task; the sub-agent completes it with its own
// context and the parent receives the sub-agent's final reply.
// A client whose chat blocks until the test releases it, so the sub-agent holds
// its slot deterministically (no sleep, no race) and the thread stays joinable.
namespace {
class WedgedClient : public agent_test::FakeLLMClient {
public:
    explicit WedgedClient(std::shared_future<void> release) : release_(std::move(release)) {}

    agent::Message chat(const std::vector<agent::Message>&,
                        const std::vector<std::shared_ptr<agent::Tool>>&, agent::Stats*) override {
        release_.wait();
        return {};
    }

private:
    std::shared_future<void> release_;
};
} // namespace

// The slot pool used to wait forever: a task that could not get a slot blocked
// the caller indefinitely, which is what "a task that never finishes" looks like
// from outside. It must now report back instead. Regression guard for the
// unbounded `slot_cv_.wait`.
TEST(subagent_slot_pool_reports_instead_of_hanging) {
    agent::Workspace::set_root(cwd());
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    executor.set_max(1);
    executor.set_slot_wait_ms(100); // a test cannot wait two minutes

    std::promise<void> release;
    const auto shared = release.get_future().share();
    executor.set_factory(
        [shared](const agent::Config&) { return std::make_unique<WedgedClient>(shared); });
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);

    // Occupy the only slot.
    std::string held_err;
    std::thread held([&] { executor.run_task("hold the slot", reg, held_err); });

    // The second task must give up and say so, not wait for the first.
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    executor.run_task("second", reg, err);
    const auto waited = std::chrono::steady_clock::now() - t0;

    release.set_value();
    held.join();

    ASSERT(!err.empty());
    ASSERT(waited < std::chrono::seconds(10));
}

TEST(subagent_usage_is_recorded_and_forwarded) {
    agent::Workspace::set_root(cwd());
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    executor.set_max(1);
    executor.set_slot_wait_ms(1000);
    agent::Config cfg = loop_cfg();
    cfg.log_path = "/tmp/amber_subagent_usage.jsonl";
    executor.set_config(cfg);

    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    agent_test::FakeReply reply;
    reply.content = "done";
    reply.prompt_tokens = 111;
    reply.completion_tokens = 22;
    script->push_back(std::move(reply));
    executor.set_factory([script](const agent::Config&) {
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        fake->script = *script;
        return fake;
    });
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);

    std::mutex mtx;
    std::vector<agent::UsageSample> samples;
    executor.set_usage_sink([&](const agent::UsageSample& sample) {
        std::scoped_lock lk(mtx);
        samples.push_back(sample);
    });

    std::string err;
    const std::string out = executor.run_task("say done", reg, err);
    ASSERT_EQ(out, std::string("done"));
    ASSERT_TRUE(err.empty());
    const auto tasks = executor.tasks();
    ASSERT_EQ(tasks.size(), 1u);
    ASSERT(tasks[0].usage_measured);
    ASSERT_EQ(tasks[0].prompt_tokens, 111L);
    ASSERT_EQ(tasks[0].completion_tokens, 22L);
    std::scoped_lock lk(mtx);
    ASSERT_EQ(samples.size(), 1u);
    ASSERT(samples[0].source == agent::UsageSource::Subagent);
    ASSERT_EQ(samples[0].prompt_tokens, 111L);
    ASSERT_EQ(samples[0].task_id, tasks[0].id);
}

TEST(subagent_task_record_tracks_owner_state_and_progress) {
    agent::Workspace::set_root(cwd());
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    executor.set_max(1);
    executor.set_slot_wait_ms(1000);
    executor.set_config(loop_cfg());
    std::promise<void> release;
    const auto shared = release.get_future().share();
    executor.set_factory(
        [shared](const agent::Config&) { return std::make_unique<WedgedClient>(shared); });
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);

    std::string owner = "window-7";
    std::string err;
    std::thread task([&] {
        agent::RunScope scope;
        scope.owner_id = &owner;
        agent::ScopedRunScope guard(scope);
        executor.run_task("a long focused task", reg, err);
    });

    bool running = false;
    for (int i = 0; i < 200 && !running; ++i) {
        const auto tasks = executor.tasks();
        running = !tasks.empty() && tasks.back().state == "running";
        if (!running)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int active_while_running = executor.active();
    const auto tasks = executor.tasks();

    // Release and join before any assertion: a failed assertion returns from
    // the test, and a still-running std::thread would terminate the process.
    release.set_value();
    task.join();

    ASSERT(running);
    ASSERT_EQ(active_while_running, 1);
    ASSERT_EQ(tasks.size(), 1u);
    ASSERT_EQ(tasks[0].owner_id, owner);
    ASSERT_EQ(tasks[0].prompt_head, std::string("a long focused task"));
    ASSERT(tasks[0].elapsed_s >= 0);
    ASSERT_EQ(executor.active(), 0);
    const auto finished = executor.tasks();
    ASSERT_EQ(finished.size(), 1u);
    ASSERT_EQ(finished[0].state, std::string("done"));
}

TEST(agent_loop_subagent_focused_task) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    // Parent turn 1: delegate. Then the sub-agent's turn: read, report. The
    // parent then closes with "done".
    push_tool_call(script, "task", {{"prompt", "read a.txt and report"}});
    push_tool_call(script, "read", {{"path", "Makefile"}});
    agent_test::FakeReply sub_done;
    sub_done.content = "sub-agent report: done reading";
    script->push_back(std::move(sub_done));
    push_text(script, "done");

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate the reading");
    ASSERT_EQ(reply, "done");
    // The task result (the sub-agent's report) must be fed back to the parent.
    bool saw_report = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" && m.content.find("sub-agent report") != std::string::npos)
            saw_report = true;
    ASSERT(saw_report);
}

// The sub-agent never exceeds its iteration cap even if the model churns:
// the task result reports the sub hit its own cap, and the parent's own
// bounded run consumed far less than the script's 200 scripted read calls.
TEST(agent_loop_subagent_iteration_cap) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.max_tool_iterations = 25; // bound the parent's churn too
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "never stop"}});
    for (int i = 0; i < 200; ++i)
        push_tool_call(script, "read", {{"path", "Makefile"}});

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate");
    // The sub-agent stopped at its own cap (message inside the task result),
    // never consuming the script's 200 reads.
    bool sub_hit_cap = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" && m.content.find("agent stopped") != std::string::npos)
            sub_hit_cap = true;
    ASSERT(sub_hit_cap);
    ASSERT(script->size() > 150u); // ~20 sub iterations consumed, not 200
    ASSERT(reply.find("stopped") != std::string::npos);
}

// Serial mode: two concurrent task calls run one at a time (cache-friendly
// request ordering — parallel requests would break shared prompt prefixes).
TEST(agent_loop_subagent_serial_mode) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    // One reply issuing BOTH task calls; the shared script serves both
    // sub-agents in order (serialized), then the parent finishes.
    agent_test::FakeReply two_calls;
    two_calls.tool_calls =
        json::array({{{"id", "call_1"},
                      {"type", "function"},
                      {"function", {{"name", "task"}, {"arguments", R"({"prompt":"first"})"}}}},
                     {{"id", "call_2"},
                      {"type", "function"},
                      {"function", {{"name", "task"}, {"arguments", R"({"prompt":"second"})"}}}}});
    script->push_back(std::move(two_calls));
    push_text(script, "report one");
    push_text(script, "report two");
    push_text(script, "done");

    auto track = std::make_shared<FakeTrack>();
    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    parent->track = track;
    executor.set_factory([script, track](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        f->track = track;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);
    executor.set_parallel(false);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate two tasks");
    ASSERT_EQ(reply, "done");
    ASSERT_EQ(track->peak.load(), 1); // never overlapped
    bool saw_one = false, saw_two = false;
    for (const auto& m : ag.context().get_all()) {
        if (m.role != "tool")
            continue;
        saw_one |= m.content.find("report one") != std::string::npos;
        saw_two |= m.content.find("report two") != std::string::npos;
    }
    ASSERT(saw_one);
    ASSERT(saw_two);
}

// Parallel mode: two concurrent task calls overlap (bounded by max).
TEST(agent_loop_subagent_parallel_mode) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    // The parent's own script: two task calls then finish.
    auto parent_script = std::make_shared<std::deque<agent_test::FakeReply>>();
    agent_test::FakeReply two_calls;
    two_calls.tool_calls =
        json::array({{{"id", "call_1"},
                      {"type", "function"},
                      {"function", {{"name", "task"}, {"arguments", R"({"prompt":"first"})"}}}},
                     {{"id", "call_2"},
                      {"type", "function"},
                      {"function", {{"name", "task"}, {"arguments", R"({"prompt":"second"})"}}}}});
    parent_script->push_back(std::move(two_calls));
    push_text(parent_script, "done");

    // Each sub-agent gets its own one-round script.
    auto scripts =
        std::make_shared<std::vector<std::shared_ptr<std::deque<agent_test::FakeReply>>>>();
    for (int i = 0; i < 2; ++i) {
        auto sub_script = std::make_shared<std::deque<agent_test::FakeReply>>();
        push_text(sub_script, "sub report " + std::to_string(i + 1));
        scripts->push_back(sub_script);
    }
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto track = std::make_shared<FakeTrack>();

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = parent_script;
    parent->track = track;
    executor.set_factory([scripts, counter, track](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = (*scripts)[(*counter)++];
        f->track = track;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);
    executor.set_parallel(true);
    executor.set_max(2);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate two tasks");
    ASSERT_EQ(reply, "done");
    ASSERT_EQ(track->peak.load(), 2); // overlapped
}

// The task tool refuses to nest: a sub-agent cannot spawn its own task.
TEST(agent_loop_subagent_nesting_guard) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "go"}});
    push_tool_call(script, "task", {{"prompt", "nested"}});
    push_text(script, "report");
    push_text(script, "done");

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate");
    ASSERT_EQ(reply, "done");
    // Exactly one sub-agent ever launched: the nested task call was blocked
    // by the guard and the sub-agent recovered on its own.
    ASSERT_EQ(executor.launched(), 1);
    ASSERT(script->empty());
}

// A sub-agent must never touch the shared registry's tool set: its Agent
// constructor used to re-register skill tools bound to the sub-agent's own
// SkillCatalog, replacing the parent's bindings with ones that dangle once
// the sub-agent is destroyed (use-after-free on the next skill call).
TEST(agent_loop_subagent_does_not_touch_shared_registry) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "reply ok"}});
    push_text(script, "ok");   // the sub-agent's reply
    push_text(script, "done"); // the parent's reply

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    std::shared_ptr<agent::Tool> before = reg.find("read_skill");
    ASSERT(before != nullptr); // the parent registers its skill tools

    std::string reply = ag.run("delegate");
    ASSERT_EQ(reply, "done");

    // The sub-agent must leave the parent's bindings untouched — same
    // instance, not a replacement pointing at a dead catalog.
    ASSERT(reg.find("read_skill") == before);
    ASSERT(reg.find("list_skills") != nullptr);
    ASSERT(reg.find("write_skill") != nullptr);
}

// A sub-agent's prompt is the same assembly as the main agent's plus the
// worker directive. The directive used to be composed into the context's system
// message by the task tool and then discarded, because Agent::run rebuilds the
// prompt whenever it differs from what the renderer produces — so the worker
// ran with the main prompt and no role boundary at all.
TEST(agent_loop_subagent_prompt_carries_the_worker_directive) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "report");
    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(fake));

    agent::set_subagent_inherited(true);
    ASSERT_EQ(ag.run("do the job"), "report");
    agent::set_subagent_inherited(false);

    ASSERT(!raw->requests.empty());
    const std::string& system = raw->requests.front().front().content;
    ASSERT(system.find("## Worker directive") != std::string::npos);
    ASSERT(system.find("concise report") != std::string::npos);
}

// The same prompt, seen by the main agent, describes no worker role.
TEST(agent_loop_main_prompt_has_no_worker_directive) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "answer");
    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(fake));
    ASSERT_EQ(ag.run("hello"), "answer");
    ASSERT(!raw->requests.empty());
    ASSERT(raw->requests.front().front().content.find("## Worker directive") == std::string::npos);
}

// Through the real task tool: the delegated run's prompt carries the directive.
TEST(agent_loop_task_tool_delegates_a_worker_prompt) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{}, executor);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "reply ok"}});
    push_text(script, "ok");   // the sub-agent's reply
    push_text(script, "done"); // the parent's reply

    // Systems are collected through a shared, mutex-guarded sink: workers run
    // on their own threads and their clients die with them.
    struct Seen {
        std::mutex mtx;
        std::vector<std::string> systems;
        void add(const std::string& s) {
            std::scoped_lock lk(mtx);
            systems.push_back(s);
        }
        std::size_t with_directive() {
            std::scoped_lock lk(mtx);
            std::size_t n = 0;
            for (const auto& s : systems)
                if (s.find("## Worker directive") != std::string::npos)
                    ++n;
            return n;
        }
        std::size_t total() {
            std::scoped_lock lk(mtx);
            return systems.size();
        }
    };
    auto seen = std::make_shared<Seen>();

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    parent->on_system = [seen](const std::string& s) { seen->add(s); };
    executor.set_factory([script, seen](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        f->on_system = [seen](const std::string& s) { seen->add(s); };
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(parent));
    ASSERT_EQ(ag.run("delegate"), "done");

    // The worker's request carries the directive; the parent's does not.
    ASSERT(seen->with_directive() >= 1u);
    ASSERT(seen->with_directive() < seen->total());
}

// [item-5] Cancellation must stop the run cleanly: no error text may be pushed
// into the session context — the stop is deliberate, not a failure.
TEST(agent_loop_cancel_does_not_fabricate_message) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply f;
    f.error = "server down";
    f.retryable = true;
    fake->script.push_back(std::move(f));

    std::thread canceller([&cfg]() {
        usleep(150 * 1000);
        cfg.cancel_token.request();
    });
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("hi");
    canceller.join();

    for (const auto& m : ag.context().get_all()) {
        ASSERT(m.content.find("[error during generation") == std::string::npos);
        ASSERT(!m.harness); // nothing to report: the stop was deliberate
    }
    (void)reply;
}

// ---------------------------------------------------------------------------
// Session brief — hermetic compression + re-injection (RED)
// Spec: docs/plan/session-brief.md
// ---------------------------------------------------------------------------

// [SB-08] When compression fires, the extract response's brief field is
// parsed, the SessionBriefStore is updated, and the brief is re-injected
// into the post-compression context as a [session-brief] message.
TEST(session_brief_extracted_on_compression) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    // Large window so warm-up turns stay below threshold; the 16 KB big
    // prompt pushes it over, firing compression exactly once (on the big
    // prompt turn). This ensures the scripted JSON replies are consumed
    // by the compression call, not by warm-up chat calls.
    cfg.context_size = 8000;
    cfg.compression_threshold = 0.5;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    // Warm-up turns (cooldown default is 20 turns before the gate opens).
    for (int i = 0; i < 21; ++i) {
        push_text(*fake, "warm");
    }
    // The compression turn: 1) classify, 2) extract (with brief), 3) generation.
    {
        agent_test::FakeReply r;
        r.content = R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
                    R"("memories":[],"skills":[],"brief":{)"
                    R"("intent":"fix parser crash","direction":"add null check",)"
                    R"("done":["found bug"],"next":"write test",)"
                    R"("avoid":["rewrite parser"]}})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[],"brief":{)"
                     R"("intent":"fix parser crash","direction":"add null check",)"
                     R"("done":["found bug"],"next":"write test",)"
                     R"("avoid":["rewrite parser"]}})");
    push_text(*fake, "hello after compression");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i)
        ag.run("warm " + std::to_string(i));

    std::string big_prompt(16000, 'x');
    ag.run(big_prompt);

    // The brief store was updated with the extracted brief.
    auto* brief_store = ag.session_brief_store();
    ASSERT(brief_store != nullptr);
    ASSERT(!brief_store->empty());
    std::string rendered = brief_store->render();
    ASSERT(rendered.find("fix parser crash") != std::string::npos);
    ASSERT(rendered.find("add null check") != std::string::npos);
    ASSERT(rendered.find("found bug") != std::string::npos);
    ASSERT(rendered.find("write test") != std::string::npos);
    ASSERT(rendered.find("rewrite parser") != std::string::npos);
}

// [SB-09] A brief seeded before compression survives the compression cycle:
// the [session-brief] message is present in the post-compression context
// and matches the store.
TEST(session_brief_survives_compression) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 8000;
    cfg.compression_threshold = 0.5;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    for (int i = 0; i < 21; ++i) {
        push_text(*fake, "warm");
    }
    // Compression turn: classify + extract return a brief that refines the
    // seeded one.
    {
        agent_test::FakeReply r;
        r.content = R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
                    R"("memories":[],"skills":[],"brief":{)"
                    R"("intent":"refined intent","direction":"refined direction",)"
                    R"("done":["step1"],"next":"refined next",)"
                    R"("avoid":["dead end A"]}})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[],"brief":{)"
                     R"("intent":"refined intent","direction":"refined direction",)"
                     R"("done":["step1"],"next":"refined next",)"
                     R"("avoid":["dead end A"]}})");
    push_text(*fake, "after");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), {}, {}, std::move(fake));

    // Seed the brief store before compression fires.
    auto* brief_store = ag.session_brief_store();
    ASSERT(brief_store != nullptr);
    agent::SessionBrief seed;
    seed.intent = "seeded intent";
    seed.direction = "seeded direction";
    seed.next = "seeded next";
    brief_store->merge(seed);

    for (int i = 0; i < 21; ++i)
        ag.run("warm " + std::to_string(i));
    std::string big_prompt(16000, 'x');
    ag.run(big_prompt);

    // After compression, the brief was refined (replaced) by the extract.
    std::string rendered = brief_store->render();
    ASSERT(rendered.find("refined intent") != std::string::npos);
    ASSERT(rendered.find("seeded intent") == std::string::npos);
    // Hash chain intact after the clear+push rebuild.
    (void)ag.context().get_all();
}

// ---------------------------------------------------------------------------
// Prompt assembly: every injected block must survive a compressing turn.
//
// The rebuild replaces the prompt copy, so any block injected BEFORE the gate
// is silently discarded — and compression fires exactly on long sessions,
// where retrieved memories matter most. These tests pin the contract for both
// turn types: a block present on a normal turn must also be present when the
// gate fires. (The suite that follows the migration asserts it for every
// block, not just memories.)
// ---------------------------------------------------------------------------

namespace {

// A gate that always fires: the compressing path is exercised on the very
// first turn instead of after the cooldown of warm-up turns the real gate
// needs, which keeps these tests fast and deterministic.
class AlwaysCompressGate : public agent::CompressionGate {
public:
    bool should_compress(const agent::Context&, const agent::Config&) const override {
        return true;
    }
};

// A gate that never fires: the control case. A fix for the compressing turn
// must not cost anything on the ordinary one.
class NeverCompressGate : public agent::CompressionGate {
public:
    bool should_compress(const agent::Context&, const agent::Config&) const override {
        return false;
    }
};

// The heaviest rebuild the pipeline can produce — everything but the system
// prompt is dropped — so a block injected before the gate cannot survive by
// accident.
class SystemOnlyCompressor : public agent::CompressionStrategy {
public:
    std::vector<agent::Message> compress(const agent::Context& context,
                                         const agent::CompressionConfig&, agent::LLMClient&,
                                         agent::CompressionObserver* = nullptr,
                                         agent::CompressionResponse* = nullptr,
                                         const agent::CompressionRequestPrefix& = {}) override {
        std::vector<agent::Message> out;
        for (const auto& m : context.get_all())
            if (m.role == "system")
                out.push_back(m);
        if (out.empty()) {
            agent::Message sys;
            sys.role = "system";
            sys.content = "system";
            out.push_back(sys);
        }
        return out;
    }
};

// A store holding one retrievable memory, plus the retriever that finds it.
struct MemoryFixture {
    agent::ExperienceConfig cfg;
    std::unique_ptr<agent::MemoryStore> store = agent::make_memory_store(cfg);

    MemoryFixture() {
        store->set_current_turn(1);
        agent::Memory mem;
        mem.content = "the build system is make";
        mem.tags = {"build"};
        mem.evidence_count = 3;
        mem.promoted = true;
        store->upsert(mem);
    }
};

// Did the model actually receive `needle` on any request?
bool request_contains(const agent_test::FakeLLMClient& fake, const std::string& needle) {
    for (const auto& req : fake.requests)
        for (const auto& m : req)
            if (m.content.find(needle) != std::string::npos)
                return true;
    return false;
}

} // namespace

TEST(agent_keeps_injected_blocks_when_compression_fires) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    MemoryFixture mem;

    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem.store);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "the build system is make");
    push_text(*fake, "done");

    auto gate = std::make_unique<AlwaysCompressGate>();
    auto compressor = std::make_unique<SystemOnlyCompressor>();
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), std::move(mem.store),
                    std::move(retriever), std::move(fake));

    ag.run("how do I build this?");

    // The rebuild replaced the prompt copy. The memory must still reach the
    // model: it is injected fresh for every turn, so a compressing turn is no
    // different from any other.
    ASSERT(request_contains(*raw, "the build system is make"));
}

// The compression cooldown is expressed in real user prompts. A turn that runs
// two tool rounds is three LLM calls; it must still count as one turn, or the
// cooldown expires inside the turn it was meant to cover.
TEST(agent_counts_user_turns_not_llm_calls) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", json{{"path", "a.txt"}});
    push_tool_call(*fake, "read", json{{"path", "a.txt"}}, "c2");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.run("read twice");

    ASSERT_EQ(ag.user_turn_counter(), size_t{1});
}

TEST(agent_compression_replays_the_injected_prefix_and_tools) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.compression_threshold = 0.7;
    cfg.compression_threshold_explicit = true;
    agent::ToolRegistry reg;
    reg.register_tool(std::make_unique<PrefixProbeTool>());
    MemoryFixture mem;

    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem.store);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, R"({"summary":"work state","classification":[{"turns":"0-0","tag":"core"}]})");
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "done");

    auto gate = std::make_unique<AlwaysCompressGate>();
    auto compressor = agent::make_compressor();
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), std::move(mem.store),
                    std::move(retriever), std::move(fake));

    ag.run("how do I build this?");

    std::size_t idx = raw->requests.size();
    for (std::size_t i = 0; i < raw->requests.size(); ++i)
        for (const auto& m : raw->requests[i])
            if (m.content.find("WORK-STATE SUMMARY") != std::string::npos)
                idx = i;
    ASSERT(idx < raw->requests.size());
    const auto& req = raw->requests[idx];
    // [system][memory block][conversation...]
    ASSERT(req.size() >= 3);
    ASSERT_EQ(req[1].role, std::string("system"));
    ASSERT(req[1].content.find("the build system is make") != std::string::npos);
    ASSERT_EQ(req[2].content, std::string("how do I build this?"));
    ASSERT_EQ(raw->tool_counts[idx], size_t{1});
}

TEST(agent_injects_in_memory_blocks_without_compression) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    MemoryFixture mem;

    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem.store);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "the build system is make");
    push_text(*fake, "done");

    auto gate = std::make_unique<NeverCompressGate>();
    auto compressor = std::make_unique<SystemOnlyCompressor>();
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), std::move(mem.store),
                    std::move(retriever), std::move(fake));

    ag.run("how do I build this?");

    // The control case: this holds today, and must keep holding.
    ASSERT(request_contains(*raw, "the build system is make"));
}

// A System block is part of the system prompt itself, not a message after it:
// tool documentation describes the harness, so it belongs in the stable prefix
// the server caches rather than at the end of the conversation.
TEST(agent_places_system_blocks_inside_the_system_prompt) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "done");

    agent::PromptRegistry prompts;
    prompts.add(
        "tool_search", "search_doc", 500,
        [] { return std::string("## search\n\nSYSTEM-BLOCK-MARKER"); },
        agent::PromptPlacement::System);

    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.set_prompt_registry(prompts);
    ag.run("go");

    ASSERT(!raw->requests.empty());
    const auto& req = raw->requests.front();
    ASSERT(!req.empty());
    ASSERT_EQ(req.front().role, std::string("system"));
    // In the system message itself: tool documentation describes the harness,
    // so it belongs in the stable prefix, next to the schema it documents.
    ASSERT(req.front().content.find("SYSTEM-BLOCK-MARKER") != std::string::npos);
    // Not smuggled in as a later system message.
    for (std::size_t i = 1; i < req.size(); ++i)
        ASSERT(req[i].content.find("SYSTEM-BLOCK-MARKER") == std::string::npos);
}

// The tools prompt is now assembled from the cross-tool part of tools.md plus
// one System block per tool plugin. That split is only safe if it is invisible:
// the assembled section must be exactly the text that was there before, byte
// for byte. The fixture is that text, captured at the moment of the split, so
// this fails the day a section moves, reorders, or loses a blank line.
TEST(tool_doc_blocks_reproduce_the_prompt_they_replaced) {
    std::ifstream in("tests/fixtures/tools_prompt_at_migration.md");
    std::stringstream fixture_stream;
    fixture_stream << in.rdbuf();
    const std::string fixture = fixture_stream.str();
    ASSERT(!fixture.empty());

    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::PromptRegistry prompts;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents, &prompts);

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.set_prompt_registry(prompts);
    ag.run("go");

    ASSERT(!raw->requests.empty());
    const std::string& system = raw->requests.front().front().content;

    // Trailing whitespace is the one thing the assembler normalises; content,
    // order and blank lines are not its to change.
    std::string expected = fixture;
    while (!expected.empty() && expected.back() == '\n')
        expected.pop_back();
    ASSERT(system.find(expected) != std::string::npos);

    // The seams: each section present once, in reading order.
    const std::size_t search = system.find("\n## search\n");
    const std::size_t read = system.find("\n## read\n");
    const std::size_t bash = system.find("\n## bash\n");
    const std::size_t process = system.find("\n## process_start\n");
    ASSERT(search != std::string::npos && read != std::string::npos);
    ASSERT(bash != std::string::npos && process != std::string::npos);
    ASSERT(search < read && read < bash && bash < process);
    ASSERT(system.rfind("\n## search\n") == search); // exactly one
}

// Switching a tool plugin off takes its documentation with it, in the same
// session: the schema follows at once, and prose describing a tool the model
// cannot call is worse than no prose at all.
TEST(disabling_a_tool_plugin_removes_its_documentation) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::HostServices host{&jobs, &todos, &subagents, &cfg.cancel_token};
    agent::Workspace ws;
    agent::PluginRuntime runtime(reg, cfg, ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start(false);

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "one");
    push_text(*fake, "two");
    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.set_prompt_registry(runtime.prompts());
    ag.run("go");

    ASSERT((bool)reg.find("search"));
    ASSERT(raw->requests.back().front().content.find("\n## search\n") != std::string::npos);

    // apply_state, not set_state: a test must not write the developer's
    // plugin.conf. set_state persists, and that is how the search tool went
    // missing from a real config.
    ASSERT_TRUE(runtime.apply_state("tool_search", false));
    ag.run("again");

    const std::string& system = raw->requests.back().front().content;
    ASSERT(system.find("\n## search\n") == std::string::npos);
    // Only that section went: its neighbours are still documented, so this is a
    // targeted removal and not a rebuild that lost text.
    ASSERT(system.find("\n## read\n") != std::string::npos);
    ASSERT(system.find("\n## bash\n") != std::string::npos);
    ASSERT_FALSE((bool)reg.find("search"));
}

// The discovery block advertises skills as "activate with read_skill", so it is
// emitted only while that tool is installed — the same rule the tool prose
// follows: the model is never told about a tool it cannot call.
TEST(discovery_block_follows_the_read_skill_tool) {
    agent::Workspace::set_root(workspace_with_one_skill("discovery-gate"));
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);

    const std::string header = "Available skills (activate with read_skill):";
    auto first_request = [&]() {
        auto fake = std::make_unique<agent_test::FakeLLMClient>();
        agent_test::FakeLLMClient* raw = fake.get();
        push_text(*fake, "done");
        agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
        ag.run("go");
        ASSERT(!raw->requests.empty());
        return raw->requests.front();
    };

    // Control: with the skill tools installed, the demo skill is advertised.
    bool advertised = false;
    for (const auto& msg : first_request())
        advertised = advertised || msg.content.find(header) != std::string::npos;
    ASSERT(advertised);

    // Gate: remove the activation tool, and the block goes with it.
    ASSERT_TRUE(reg.remove_tool("read_skill"));
    for (const auto& msg : first_request())
        ASSERT(msg.content.find(header) == std::string::npos);
}

// The point of the plugin boundary: a user who wants the harness lean switches
// a plugin off and the app keeps running. The tools, their prose and the
// discovery block leave together, a neighbour's tool still works, and the
// audit has nothing to warn about.
TEST(agent_runs_a_turn_with_the_skill_plugin_disabled) {
    agent::Workspace::set_root(workspace_with_one_skill("skills-off"));
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::HostServices host{&jobs, &todos, &subagents, &cfg.cancel_token};
    agent::Workspace ws;
    agent::PluginRuntime runtime(reg, cfg, ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    // apply_state, not set_state: a test must not write the developer's
    // plugin.conf (state is what set_state persists).
    ASSERT_TRUE(runtime.apply_state("tool_skills", false));
    ASSERT_FALSE(runtime.status("tool_skills").enabled);
    ASSERT_FALSE((bool)reg.find("read_skill"));
    ASSERT_FALSE((bool)reg.find("list_skills"));
    ASSERT_FALSE((bool)reg.find("write_skill"));

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "bash", {{"command", "echo x"}});
    push_text(*fake, "done");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, auto_approve_hooks(), {}, {}, {}, {}, std::move(fake));
    ag.set_prompt_registry(runtime.prompts());
    ASSERT_EQ(ag.run("do it"), "done");

    // The rest of the harness is intact: a neighbour ran, and read/file
    // reading is still covered (the skill tools are not the only readers).
    ASSERT(!raw->requests.empty());
    ASSERT(raw->tool_counts.front() >= 1u);
    ASSERT(runtime.audit().empty());

    const std::string& system = raw->requests.front().front().content;
    // Neither the skills prose nor the discovery block is in the prompt.
    ASSERT(system.find("Skills are reusable procedures") == std::string::npos);
    ASSERT(system.find("Available skills (activate with read_skill):") == std::string::npos);
    // Only that plugin's documentation left.
    ASSERT(system.find("\n## bash\n") != std::string::npos);
}

// Toggling a tool plugin changes the schema at once, so the documentation must
// follow it in the same session. This is the invariant §3.4 of the tools spec
// exists for: the model is never told about a tool it cannot call.
TEST(agent_rebuilds_the_system_prompt_when_a_contribution_goes_away) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (const char* reply : {"one", "two", "three"})
        push_text(*fake, reply);

    agent::PromptRegistry prompts;
    auto doc = prompts.add(
        "tool_search", "search_doc", 500, [] { return std::string("SEARCH-DOC-MARKER"); },
        agent::PromptPlacement::System);

    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.set_prompt_registry(prompts);
    ag.run("first");

    ASSERT(raw->requests.size() >= 1u);
    ASSERT(raw->requests.back().front().content.find("SEARCH-DOC-MARKER") != std::string::npos);

    // A further turn with nothing changed leaves the prompt alone: still one
    // system message, still carrying the doc.
    ag.run("second");
    auto system_messages = [&ag] {
        std::size_t n = 0;
        for (const auto& m : ag.context().get_all())
            if (m.role == "system")
                ++n;
        return n;
    };
    ASSERT_EQ(system_messages(), 1u);

    // The plugin goes away: schema and prose leave together, and the rebuild
    // replaces the system message rather than adding another.
    doc.remove();
    ag.run("third");
    ASSERT_EQ(system_messages(), 1u);

    const auto& req = raw->requests.back();
    ASSERT(req.front().role == std::string("system"));
    ASSERT(req.front().content.find("SEARCH-DOC-MARKER") == std::string::npos);
    // The conversation survived the rebuild.
    bool saw_first_turn = false;
    for (const auto& m : req)
        if (m.role == "user" && m.content == "first")
            saw_first_turn = true;
    ASSERT(saw_first_turn);
}

// The tail default is unchanged: a block with no declared placement still ends
// up after the conversation, which is what every existing contributor expects.
TEST(agent_keeps_tail_placement_as_the_default) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "done");

    agent::PromptRegistry prompts;
    prompts.add("plug", "note", 1000, [] { return std::string("TAIL NOTE"); });

    agent::Agent ag(cfg, reg, {}, nullptr, nullptr, nullptr, nullptr, std::move(fake));
    ag.set_prompt_registry(prompts);
    ag.run("go");

    ASSERT(!raw->requests.empty());
    const auto& req = raw->requests.front();
    ASSERT_EQ(req.back().content, std::string("TAIL NOTE"));
    ASSERT(req.front().content.find("TAIL NOTE") == std::string::npos);
}

TEST(agent_orders_injected_blocks_as_documented) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    MemoryFixture mem;

    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem.store);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "the build system is make");
    push_text(*fake, "done");

    agent::PromptRegistry prompts;
    prompts.add("plug", "tail", 400, [] { return std::string("PLUGIN TAIL"); });

    auto gate = std::make_unique<NeverCompressGate>();
    auto compressor = std::make_unique<SystemOnlyCompressor>();
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), std::move(mem.store),
                    std::move(retriever), std::move(fake));
    ag.set_prompt_registry(prompts);
    agent::SessionBrief fresh;
    fresh.intent = "wire the prompt blocks";
    ag.session_brief_store()->merge(fresh);

    ag.run("how do I build this?");

    ASSERT(!raw->requests.empty());
    const auto& req = raw->requests.front();

    // Head: the base prompt, then the injected blocks that read as
    // instructions, in priority order — memory (100) before discovery (200).
    ASSERT(!req.empty());
    ASSERT_EQ(req.front().role, std::string("system"));
    ASSERT(req[1].content.find("the build system is make") != std::string::npos);

    // Tail: after the conversation, so a change costs the KV cache only from
    // there on. Order is activated bodies (900), brief (950), plugins (1000).
    ASSERT_EQ(req.back().content, std::string("PLUGIN TAIL"));
    bool brief_seen = false;
    bool brief_before_plugin = false;
    for (const auto& m : req) {
        if (m.content.find("session-brief") != std::string::npos) {
            brief_seen = true;
            brief_before_plugin = true;
        } else if (m.content == "PLUGIN TAIL") {
            break;
        }
    }
    ASSERT(brief_seen);
    ASSERT(brief_before_plugin);

    // The tail block really is after the conversation, not mixed into it.
    ASSERT(req[req.size() - 2].content.find("session-brief") != std::string::npos);
}

TEST(agent_places_injected_blocks_after_the_system_prompt) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    MemoryFixture mem;

    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem.store);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "the build system is make");
    push_text(*fake, "done");

    auto gate = std::make_unique<AlwaysCompressGate>();
    auto compressor = std::make_unique<SystemOnlyCompressor>();
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate), std::move(mem.store),
                    std::move(retriever), std::move(fake));

    ag.run("how do I build this?");

    // Ordering is part of the contract the KV prefix depends on: the injected
    // block sits with the system prompt, not after the conversation.
    ASSERT(!raw->requests.empty());
    const auto& req = raw->requests.front();
    ASSERT(!req.empty());
    ASSERT_EQ(req.front().role, std::string("system"));
    bool memory_before_conversation = false;
    for (const auto& m : req) {
        if (m.content.find("the build system is make") != std::string::npos) {
            memory_before_conversation = true;
            break;
        }
        if (m.role == "user")
            break; // reached the conversation without seeing it
    }
    ASSERT(memory_before_conversation);
}

// Disabling a plugin can strip the registry down to nothing it contributed (the
// core tool set is a plugin contribution). That has to read as an agent that
// cannot act, not as a hang: the turn still completes, and the request
// advertises no tools rather than failing to build.
TEST(agent_runs_with_an_empty_tool_registry) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg; // deliberately empty
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "I have no tools");
    push_text(*fake, "done");

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    const std::string reply = ag.run("what can you do?");

    ASSERT_EQ(reply, "I have no tools");
    ASSERT(!raw->requests.empty());
    ASSERT_EQ(raw->tool_counts.front(), static_cast<std::size_t>(0));
}

// The skill tools arrive through the host's install path, never from the Agent
// itself: a bare host that calls register_default_tools gets the same three
// capabilities the tool_skills plugin declares (one definition, two paths).
TEST(bare_install_provides_the_skill_tools) {
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(reg, jobs, todos, cfg.cancel_token, subagents);

    ASSERT_TRUE((bool)reg.find("read_skill"));
    ASSERT_TRUE((bool)reg.find("list_skills"));
    ASSERT_TRUE((bool)reg.find("write_skill"));
}

// ---------------------------------------------------------------------------
// FIX-033 (RED): a cancel requested while a tool is mid-execution must reach
// that tool on its dispatch worker. Today the worker has no RunScope, so the
// tool only ever sees the registration-bound fallback and never observes it.
// ---------------------------------------------------------------------------

namespace {

class WaitCancelProbe : public agent::Tool {
public:
    std::string name() const noexcept override { return "wait_cancel"; }
    bool is_read_only() const noexcept override { return true; }
    std::string description() const noexcept override { return "waits for cancellation"; }
    agent::json parameters_schema() const override {
        return {{"type", "object"}, {"properties", agent::json::object()}};
    }

    agent::ToolResult execute(const agent::json&) const override {
        agent::CancellationToken inert; // never requested
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
        while (!agent::run_cancelled(inert) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        observed.store(agent::run_cancelled(inert));
        agent::ToolResult r;
        r.ok = true;
        r.output = "probe done";
        return r;
    }

    mutable std::atomic<bool> observed{false};
};

} // namespace

TEST(agent_loop_cancel_reaches_in_flight_tool) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    auto probe = std::make_unique<WaitCancelProbe>();
    WaitCancelProbe* p = probe.get();
    reg.register_tool(std::move(probe));

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "wait_cancel", {{"x", 1}});
    push_text(*fake, "done");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ag.request_cancel();
    });
    ag.run("go");
    canceller.join();

    ASSERT(p->observed.load());
}

// Folding a reply into the turn is where dispatch, the tool envelope and the
// host callbacks run. A failure there is not a chat failure, so it had no
// recovery path: it escaped Agent::run(), the turn died with an empty reply and
// no record. The turn must survive it — reported, seen by the model, and with
// every tool_call still answered so the next request stays valid.
TEST(agent_loop_internal_failure_is_recovered) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "carried on");
    push_text(*fake, "done");

    bool reported = false;
    agent::AgentHooks hooks;
    hooks.on_status = [&](const std::string& s) {
        if (s.find("recovered") != std::string::npos)
            reported = true;
    };
    hooks.on_tool_result = [](const std::string&, const agent::ToolResult&, const agent::json&) {
        throw std::runtime_error("host callback exploded");
    };
    agent::Agent ag(cfg, reg, hooks, {}, {}, {}, {}, std::move(fake));

    ASSERT_EQ(ag.run("read the Makefile"), std::string("carried on"));
    ASSERT(reported);
    // The model was told what broke, and the turn continued on its next reply.
    bool saw_notice = false;
    for (const auto& m : ag.context().get_all())
        if (m.content.find("host callback exploded") != std::string::npos)
            saw_notice = true;
    ASSERT(saw_notice);
    ASSERT(raw->requests.size() >= 2u);
    // The tool call the failure swallowed is still answered: a tool_call with no
    // tool message is an invalid request and poisons every later turn.
    std::size_t asked = 0, answered = 0;
    for (const auto& m : raw->requests[1]) {
        if (m.role == "assistant" && m.tool_calls.is_array())
            asked += m.tool_calls.size();
        if (m.role == "tool")
            ++answered;
    }
    ASSERT_EQ(asked, answered);
}

// An empty reply is judged by THIS turn, not by the retained history: after a
// compaction the context still holds tool messages from earlier turns, and
// scanning them made every empty reply claim it happened "after tool calls".
TEST(agent_empty_reply_is_judged_by_this_turn_not_history) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.log_path = "/dev/null";
    cfg.experience_store_path = "/dev/null";
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", {{"path", "Makefile"}}, "c1");
    push_text(*fake, "done reading");
    push_text(*fake, ""); // turn 2: a silent reply gets one nudge
    push_text(*fake, ""); // ... then the silence bound ends the turn
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ASSERT_EQ(ag.run("read it"), "done reading");
    const std::string stopped = ag.run("what else?");
    ASSERT(stopped.find("after tool calls") == std::string::npos);
    ASSERT(stopped.find("produced no usable reply") != std::string::npos);
}

// A provider outage is named in the stop reason and shown on the host's status
// channel: the observed failure was a 502 with "see the ERROR messages above"
// appearing in a transcript that contained no error line at all.
TEST(agent_failed_request_is_named_in_the_stop_and_shown) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.log_path = "/dev/null";
    cfg.experience_store_path = "/dev/null";
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    for (int i = 0; i < 3; ++i) {
        agent_test::FakeReply err;
        err.error = "HTTP 502 from LLM server: provider returned error";
        err.retryable = true;
        fake->script.push_back(std::move(err));
    }
    push_text(*fake, "");
    push_text(*fake, "");

    std::vector<std::string> status;
    agent::AgentHooks hooks;
    hooks.on_status = [&status](const std::string& line) { status.push_back(line); };
    agent::Agent ag(cfg, reg, std::move(hooks), {}, {}, {}, {}, std::move(fake));

    const std::string stopped = ag.run("suggest features");
    ASSERT(stopped.find("the last request failed") != std::string::npos);
    ASSERT(stopped.find("HTTP 502") != std::string::npos);
    bool visible = false;
    for (const auto& line : status)
        if (line.rfind("error: transport:", 0) == 0)
            visible = true;
    ASSERT(visible);
}
TEST(agent_prompt_names_a_detached_worktree) {
    namespace fs = std::filesystem;
    const std::string repo =
        (fs::temp_directory_path() / ("amber_agent_wt_repo_" + std::to_string(::getpid())))
            .string();
    fs::remove_all(repo);
    fs::create_directories(repo);
    const std::string init = "cd '" + repo +
                             "' && git init -q -b main . && echo base > f.txt && git add f.txt && "
                             "git -c user.email=t@t -c user.name=t commit -qm init";
    ASSERT_EQ(std::system(init.c_str()), 0);

    const std::string tree =
        (fs::temp_directory_path() / ("amber_agent_wt_tree_" + std::to_string(::getpid())))
            .string();
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo, tree, agent::git_head_oid(repo), err));
    ASSERT_TRUE(err.empty());

    const std::string saved = agent::Workspace::project_root();
    agent::Workspace::set_root(tree);
    agent::Config cfg = loop_cfg();
    // This test constructs a real Agent in a temp project: keep its transcript
    // and learned memory out of the developer's state.
    cfg.log_path = "/dev/null";
    cfg.experience_store_path = "/dev/null";
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_text(*fake, "ok");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.run("hi");

    ASSERT(ag.context().get_all()[0].content.find("detached git worktree") != std::string::npos);

    agent::Workspace::set_root(saved);
    std::string cleanup_err;
    ASSERT_TRUE(agent::remove_worktree_safe(repo, tree, cleanup_err));
    fs::remove_all(repo);
}
