
// Hermetic agent-loop tests: the real Agent + real Context + real registry,
// driven by the scripted FakeLLMClient. No network, no server.
// Scenarios map to docs/spec/llm-client/agent-loop-reliability.md [AL-xx].

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "agent.h"
#include "agent/tools.h"
#include "fake_llm.h"
#include "tests/test_util.h"

namespace {

std::string cwd() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : ".";
}

agent::Config loop_cfg() {
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 100;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    return cfg;
}

void push_text(agent_test::FakeLLMClient& fake, const std::string& text) {
    agent_test::FakeReply r;
    r.content = text;
    fake.script.push_back(std::move(r));
}

void push_tool_call(agent_test::FakeLLMClient& fake, const std::string& fn,
                    const json& args, const std::string& id = "call_1") {
    agent_test::FakeReply r;
    r.tool_calls = json::array(
        {{{"id", id},
          {"type", "function"},
          {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    fake.script.push_back(std::move(r));
}

} // namespace

// [AL-01] A plain text reply is returned; the confirmation probe follows.
TEST(agent_loop_plain_reply) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "hello there");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "hello there");
    ASSERT_EQ(raw->chat_calls, 2);  // generation + confirmation probe
    // Context: system, user, assistant, probe prompt, probe ack.
    const auto& ctx = ag.context().get_all();
    ASSERT_EQ(ctx.size(), 5u);
    bool saw_user = false;
    for (const auto& m : ctx)
        if (m.role == "user" && m.content == "hi") saw_user = true;
    ASSERT(saw_user);
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("read the Makefile");
    ASSERT_EQ(reply, "done reading");
    // The tool result must be in the conversation (fed back to the model).
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" &&
            m.content.find("Makefile") != std::string::npos)
            saw_tool_result = true;
    ASSERT(saw_tool_result);
    // Second request carries the tool message back to the model.
    ASSERT(raw->requests.size() >= 2u);
    bool fed_back = false;
    for (const auto& m : raw->requests[1])
        if (m.role == "tool") fed_back = true;
    ASSERT(fed_back);
}

// [AL-03] Confirmation probe dispatches tool calls (the use-after-move
// regression: moved-from tool_calls used to silently kill this path).
TEST(agent_loop_confirmation_dispatches_tools) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "let me check");
    push_tool_call(*fake, "read", {{"path", "Makefile"}}, "probe_1");
    push_text(*fake, "verified");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("check the build file");
    ASSERT_EQ(reply, "verified");
    // The probe's tool call was dispatched: a tool result is in context.
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" &&
            m.content.find("Makefile") != std::string::npos)
            saw_tool_result = true;
    ASSERT(saw_tool_result);
    ASSERT(raw->chat_calls == 4);
}

// [AL-04] The loop stops at max_tool_iterations.
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
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("read many files");
    ASSERT_FALSE(reply.empty());  // graceful fallback, not a crash
    ASSERT_EQ(raw->chat_calls, 3);
    ASSERT(raw->requests.size() == 3u);
}

// [AL-05] Repeated identical text triggers the steer, then the hard stop.
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
        if (s.find("text loop") != std::string::npos) steered = true;
    };
    // Six repeats of "foo", each followed by a probe that dispatches a tool
    // (so the confirmation never accepts). The steer fires at repeat 2; the
    // hard stop at repeat 6 (the steer no longer resets the counter).
    const char* paths[] = {"Makefile",      "Makefile.in", "lib/llm.cpp",
                           "lib/agent.cpp", "tests/run_tests.cpp",
                           "include/agent/agent.h"};
    for (int i = 0; i < 6; ++i) {
        push_text(*fake, "foo");
        push_tool_call(*fake, "read", {{"path", paths[i]}},
                       "probe_" + std::to_string(i));
    }
    agent::Agent ag(cfg, reg, std::move(hooks), {}, {}, {}, {},
                    std::move(fake));

    std::string reply = ag.run("do the thing");
    ASSERT(steered);
    ASSERT(reply.find("loop detected") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 11);  // 6th repeat hard-stops before its probe
}

// [AL-06] The compression gate fires once the cooldown window passes; the
// context is rebuilt via clear+push and the hash chain stays intact.
TEST(agent_loop_compression_trigger) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 2000;
    cfg.compression_threshold = 0.5;
    cfg.compression_min_turns = 2;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    // Warm-up turns (cooldown default is 20 turns before the gate opens).
    for (int i = 0; i < 21; ++i) {
        push_text(*fake, "warm");
        push_text(*fake, "done");
    }
    // The compression turn: 1) classify, 2) extract, 3) generation, 4) probe.
    {
        agent_test::FakeReply r;
        r.content =
            R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
            R"("memories":[],"skills":[]})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "hello after compression");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate),
                    {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i) ag.run("warm " + std::to_string(i));

    std::string big_prompt(8000, 'x');
    std::string reply = ag.run(big_prompt);
    ASSERT_EQ(reply, "hello after compression");
    // The classification request reached the client somewhere in the flow.
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("Classify ALL turn ranges") !=
                std::string::npos)
                saw_compression = true;
    ASSERT(saw_compression);
    // Hash chain intact after the clear+push rebuild.
    (void)ag.context().get_all();
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.run("read the Makefile");
    // Multiple reads over the live stack; each get_all() verifies the chain.
    (void)ag.context().get_all();
    ag.run("and again");
    (void)ag.context().get_all();
    ASSERT(raw->chat_calls >= 4);
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "ok after retries");
    ASSERT_EQ(raw->chat_calls, 4);  // 3 attempts + confirmation probe
}

// [AL-08] A non-retryable error fails fast (single attempt).
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT(reply.find("error during") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 2);  // 1 attempt + probe; no retries
}

// [AL-09] Retries exhausted -> graceful error reply, conversation intact.
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("hi");
    ASSERT(reply.find("error during") != std::string::npos);
    ASSERT_EQ(raw->chat_calls, 4);  // 3 attempts + probe
    // The user prompt is preserved for a manual retry.
    const auto& ctx = ag.context().get_all();
    bool saw_user = false;
    for (const auto& m : ctx)
        if (m.role == "user" && m.content == "hi") saw_user = true;
    ASSERT(saw_user);
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
    push_text(*fake, "done");

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
    ASSERT(reply.find("cancelled") != std::string::npos);
    // Only the first generation attempt ran (the backoff wait was aborted);
    // the second call is the loop's confirmation probe, not a retry.
    ASSERT_EQ(raw->chat_calls, 2);
    ASSERT(elapsed.count() < 900);
}

// [AL-11] Unknown n_ctx: the gate falls back to a conservative budget instead
// of disabling compression entirely.
TEST(agent_loop_unknown_context_fallback) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 0;  // server never reported n_ctx
    cfg.compression_threshold = 0.1;
    cfg.compression_min_turns = 2;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 21; ++i) {
        push_text(*fake, "warm");
        push_text(*fake, "done");
    }
    {
        agent_test::FakeReply r;
        r.content =
            R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
            R"("memories":[],"skills":[]})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "after fallback compression");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate),
                    {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i) ag.run("warm " + std::to_string(i));

    std::string big_prompt(8000, 'x');
    std::string reply = ag.run(big_prompt);
    ASSERT_EQ(reply, "after fallback compression");
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("Classify ALL turn ranges") !=
                std::string::npos)
                saw_compression = true;
    ASSERT(saw_compression);
    (void)ag.context().get_all();
}
