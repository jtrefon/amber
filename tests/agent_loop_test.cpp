
// Hermetic agent-loop tests: the real Agent + real Context + real registry,
// driven by the scripted FakeLLMClient. No network, no server.
// Scenarios map to docs/spec/llm-client/agent-loop-reliability.md [AL-xx].

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <atomic>

#include "agent.h"
#include "agent/tools.h"
#include "agent/todo.h"
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

void push_text(const std::shared_ptr<std::deque<agent_test::FakeReply>>& script,
               const std::string& text) {
    agent_test::FakeReply r;
    r.content = text;
    script->push_back(std::move(r));
}

void push_tool_call(const std::shared_ptr<std::deque<agent_test::FakeReply>>& script,
                    const std::string& fn, const json& args,
                    const std::string& id = "call_1") {
    agent_test::FakeReply r;
    r.tool_calls = json::array(
        {{{"id", id},
          {"type", "function"},
          {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    script->push_back(std::move(r));
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
    // [I-9] The environment card rides in the system message.
    ASSERT(ctx[0].content.find("## Environment") != std::string::npos);
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
    cfg.context_size = 262144;  // configured from model metadata (trained)
    cfg.compression_threshold = 0.7;
    cfg.compression_min_turns = 2;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto fake = std::make_unique<LearnedFake>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 21; ++i) {
        push_text(*fake, "warm");
        push_text(*fake, "done");
    }
    // One warm reply reports 95k prompt tokens: 72% of the LEARNED 131072
    // window (fire) but only 36% of the configured 262144 (no fire without
    // the clamp).
    {
        agent_test::FakeReply r;
        r.prompt_tokens = 95000;
        fake->script.push_back(std::move(r));
    }
    {
        agent_test::FakeReply r;
        r.content =
            R"({"classification":[{"turns":"0-0","tag":"core","summary":""}],)"
            R"("memories":[],"skills":[]})";
        fake->script.push_back(std::move(r));
    }
    push_text(*fake, R"({"memories":[],"skills":[]})");
    push_text(*fake, "compressed after learned clamp");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate),
                    {}, {}, std::move(fake));
    for (int i = 0; i < 21; ++i) ag.run("warm " + std::to_string(i));

    std::string reply = ag.run("next");
    ASSERT_EQ(reply, "compressed after learned clamp");
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("Classify ALL turn ranges") !=
                std::string::npos)
                saw_compression = true;
    ASSERT(saw_compression);
}

// The pipeline must forward BOTH the classification segments and the
// memory/skill ops to the caller — previously only the ops survived, so the
// reported core/context/prune counts were always zero.
TEST(compression_pipeline_forwards_segments_and_ops) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor(cc);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply classify;
    classify.content =
        R"({"classification":[{"turns":"0-1","tag":"context","summary":"greeting"}],)"
        R"("memories":[],"skills":[]})";
    fake->script.push_back(std::move(classify));
    agent_test::FakeReply extract;
    extract.content =
        R"({"memories":[{"name":"fact-1","content":"a fact","action":"upsert"}],)"
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
    // The classify/extract requests were popped again — context unchanged.
    ASSERT_EQ(ctx.size(), 2);
    (void)ctx.get_all();  // hash chain intact after the push/pop pairs
}

// Spec invariant 7: if any LLM call or parse fails, the input history is
// returned unchanged. The loop-collapse pass must not leak into the failure
// path — previously the caller rebuilt the context from the collapsed copy.
TEST(compression_pipeline_failure_returns_history_unchanged) {
    agent::CompressionConfig cc;
    auto compressor = agent::make_compressor(cc);
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
        ctx.push(agent_test::tool_call_msg(
            "search", json{{"pattern", "x"}}));
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
// An unknown context window disables AUTO-compression (no guessed budget —
// an arbitrary fallback fired at a tiny fraction of modern windows). Manual
// /compress and the 400-overflow learner still cover it.
TEST(agent_loop_unknown_context_never_auto_fires) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.context_size = 0;  // server never reported n_ctx
    cfg.compression_threshold = 0.1;
    cfg.compression_threshold_explicit = true;
    cfg.compression_min_turns = 2;
    cfg.compression_min_turns_explicit = true;
    agent::ToolRegistry reg;
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    for (int i = 0; i < 5; ++i) {
        push_text(*fake, "warm");
        push_text(*fake, "done");
    }
    push_text(*fake, "direct reply");
    agent::Agent ag(cfg, reg, {}, std::move(compressor), std::move(gate),
                    {}, {}, std::move(fake));
    for (int i = 0; i < 5; ++i) ag.run("warm " + std::to_string(i));

    std::string big_prompt(8000, 'x');
    std::string reply = ag.run(big_prompt);
    ASSERT_EQ(reply, "direct reply");
    bool saw_compression = false;
    for (const auto& req : raw->requests)
        for (const auto& m : req)
            if (m.content.find("Classify ALL turn ranges") !=
                std::string::npos)
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("hi");
    ASSERT_EQ(reply, "hello there");
    // Generation attempt 1 carried tools; the adapted retry did not; the
    // confirmation probe sent tools again.
    ASSERT_EQ(raw->tool_counts.size(), 3u);
    ASSERT(raw->tool_counts[0] > 0u);   // first attempt carried tools
    ASSERT_EQ(raw->tool_counts[1], 0u); // adapted retry dropped them
    ASSERT(raw->tool_counts[2] > 0u);   // probe carried tools again
}

// [GR] When even the adapted retry fails, the internal probe rethrows so the
// turn ends with a real error instead of faking a reply into the context.
TEST(agent_probe_failure_throws_after_recovery) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    for (int i = 0; i < 4; ++i) {
        agent_test::FakeReply fail;
        fail.error = "HTTP 400 from LLM server: Unable to generate parser "
                     "for this template";
        fail.retryable = false;
        fake->script.push_back(std::move(fail));
    }
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    bool threw = false;
    try {
        ag.run("hi");
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT(threw);
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
    r.content =
        "Let me check.\n"
        "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n"
        "</parameter>\n</function>\n</tool_call>";
    fake->script.push_back(std::move(r));
    push_text(*fake, "done reading");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("read the Makefile");
    ASSERT_EQ(reply, "done reading");
    const auto& ctx = ag.context().get_all();
    bool saw_tool_result = false;
    for (const auto& m : ctx)
        if (m.role == "tool" &&
            m.content.find("Makefile") != std::string::npos)
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
    r.content =
        "<tool_call>\n<function=read>\n<parameter=path>\nMakefile\n"
        "</parameter>\n</function>\n</tool_call>";
    fake->script.push_back(std::move(r));
    push_text(*fake, "done reading");
    push_text(*fake, "done");
    int tool_call_hook_count = 0;
    agent::AgentHooks hooks;
    hooks.on_tool_call = [&](const std::string&, const agent::json&) {
        ++tool_call_hook_count;
    };
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
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  true);
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_tool_call(*fake, "todowrite",
                   {{"todos", {{{"id", "p1"}, {"text", "fix parsing"},
                                {"status", "in_progress"}}}}});
    push_tool_call(*fake, "todowrite",
                   {{"todos", {{{"id", "p1"}, {"text", "fix parsing"},
                                {"status", "completed"}},
                               {{"id", "p2"}, {"text", "write tests"},
                                {"status", "pending"}}}}});
    push_text(*fake, "done");
    push_text(*fake, "yes");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    std::string reply = ag.run("track the work");
    ASSERT_EQ(reply, "done");
    // The final full-list replacement is what the store holds.
    ASSERT_EQ(todos.items().size(), 2u);
    ASSERT(todos.items()[0].status == agent::TodoStatus::Completed);
    ASSERT_EQ(todos.items()[1].text, "write tests");
    ASSERT(raw->chat_calls >= 3);
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
        {{{"old", ""},
          {"new",
           std::string(300, 'L')}}});  // > 120 chars -> args must not echo
    push_tool_call(*fake, "write",
                   {{"path", "bench_envelope_test.txt"}, {"edits", edits}});
    push_text(*fake, "done writing");
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.run("create bench_envelope_test.txt");
    std::remove("bench_envelope_test.txt");
    const auto& ctx = ag.context().get_all();
    bool saw_lean = false;
    for (const auto& m : ctx) {
        if (m.role != "tool") continue;
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
    push_text(*fake, "done");
    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));

    ag.run("read the Makefile");
    const auto& ctx = ag.context().get_all();
    bool saw_echo = false;
    for (const auto& m : ctx) {
        if (m.role != "tool") continue;
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

    agent::Message chat(const std::vector<agent::Message>&,
                        const std::vector<agent::Tool*>& tools,
                        agent::Stats* stats = nullptr) override {
        tool_counts.push_back(tools.size());
        if (track) {
            const int a = ++track->active;
            int p = track->peak.load();
            while (a > p && !track->peak.compare_exchange_weak(p, a)) {}
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

    agent::Message chat_stream(
        const std::vector<agent::Message>&,
        const std::vector<agent::Tool*>&,
        const std::function<void(const agent::StreamChunk&)>&,
        agent::Stats* stats = nullptr) override {
        return chat({}, {}, stats);
    }
};

// The parent delegates one task; the sub-agent completes it with its own
// context and the parent receives the sub-agent's final reply.
TEST(agent_loop_subagent_focused_task) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    // Parent turn 1: delegate. Then the sub-agent's turns: read, report,
    // probe-confirm; the parent then chats "done" and probe-confirms "yes".
    push_tool_call(script, "task", {{"prompt", "read a.txt and report"}});
    push_tool_call(script, "read", {{"path", "Makefile"}});
    agent_test::FakeReply sub_done;
    sub_done.content = "sub-agent report: done reading";
    script->push_back(std::move(sub_done));
    push_text(script, "done");
    push_text(script, "done");
    push_text(script, "yes");

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate the reading");
    ASSERT_EQ(reply, "done");
    // The task result (the sub-agent's report) must be fed back to the parent.
    bool saw_report = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" &&
            m.content.find("sub-agent report") != std::string::npos)
            saw_report = true;
    ASSERT(saw_report);
}

// The sub-agent never exceeds its iteration cap even if the model churns:
// the task result reports the sub hit its own cap, and the parent's own
// bounded run consumed far less than the script's 200 scripted read calls.
TEST(agent_loop_subagent_iteration_cap) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    cfg.max_tool_iterations = 25;  // bound the parent's churn too
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "never stop"}});
    for (int i = 0; i < 200; ++i)
        push_tool_call(script, "read", {{"path", "Makefile"}});
    push_text(script, "done");
    push_text(script, "yes");

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate");
    // The sub-agent stopped at its own cap (message inside the task result),
    // never consuming the script's 200 reads.
    bool sub_hit_cap = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" &&
            m.content.find("agent stopped") != std::string::npos)
            sub_hit_cap = true;
    ASSERT(sub_hit_cap);
    ASSERT(script->size() > 150u);  // ~20 sub iterations consumed, not 200
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
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    // One reply issuing BOTH task calls; the shared script serves both
    // sub-agents in order (serialized), then the parent finishes.
    agent_test::FakeReply two_calls;
    two_calls.tool_calls = json::array(
        {{{"id", "call_1"},
          {"type", "function"},
          {"function", {{"name", "task"}, {"arguments", R"({"prompt":"first"})"}}}},
         {{"id", "call_2"},
          {"type", "function"},
          {"function", {{"name", "task"}, {"arguments", R"({"prompt":"second"})"}}}}});
    script->push_back(std::move(two_calls));
    push_text(script, "report one");
    push_text(script, "done");
    push_text(script, "report two");
    push_text(script, "done");
    push_text(script, "done");
    push_text(script, "yes");

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

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate two tasks");
    ASSERT_EQ(reply, "done");
    ASSERT_EQ(track->peak.load(), 1);  // never overlapped
    bool saw_one = false, saw_two = false;
    for (const auto& m : ag.context().get_all()) {
        if (m.role != "tool") continue;
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
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    // The parent's own script: two task calls then finish.
    auto parent_script = std::make_shared<std::deque<agent_test::FakeReply>>();
    agent_test::FakeReply two_calls;
    two_calls.tool_calls = json::array(
        {{{"id", "call_1"},
          {"type", "function"},
          {"function", {{"name", "task"}, {"arguments", R"({"prompt":"first"})"}}}},
         {{"id", "call_2"},
          {"type", "function"},
          {"function", {{"name", "task"}, {"arguments", R"({"prompt":"second"})"}}}}});
    parent_script->push_back(std::move(two_calls));
    push_text(parent_script, "done");
    push_text(parent_script, "yes");

    // Each sub-agent gets its own one-round script.
    auto scripts = std::make_shared<std::vector<std::shared_ptr<std::deque<agent_test::FakeReply>>>>();
    for (int i = 0; i < 2; ++i) {
        auto sub_script = std::make_shared<std::deque<agent_test::FakeReply>>();
        push_text(sub_script, "sub report " + std::to_string(i + 1));
        push_text(sub_script, "done");
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

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate two tasks");
    ASSERT_EQ(reply, "done");
    ASSERT_EQ(track->peak.load(), 2);  // overlapped
}

// The task tool refuses to nest: a sub-agent cannot spawn its own task.
TEST(agent_loop_subagent_nesting_guard) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = loop_cfg();
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor executor;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "go"}});
    push_tool_call(script, "task", {{"prompt", "nested"}});
    push_text(script, "report");
    push_text(script, "done");
    push_text(script, "done");
    push_text(script, "yes");

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
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
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, executor, true);
    auto script = std::make_shared<std::deque<agent_test::FakeReply>>();
    push_tool_call(script, "task", {{"prompt", "reply ok"}});
    push_text(script, "ok");          // the sub-agent's reply
    push_text(script, "done");        // the parent's reply
    push_text(script, "done");        // parent probe
    push_text(script, "yes");         // probe confirmation

    auto parent = std::make_unique<SharedScriptFake>();
    parent->script = script;
    executor.set_factory([script](const agent::Config&) {
        auto f = std::make_unique<SharedScriptFake>();
        f->script = script;
        return std::unique_ptr<agent::LLMClient>(std::move(f));
    });
    executor.set_config(cfg);

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(parent));
    agent::Tool* before = reg.find("read_skill");
    ASSERT(before != nullptr);  // the parent registers its skill tools

    std::string reply = ag.run("delegate");
    ASSERT_EQ(reply, "done");

    // The sub-agent must leave the parent's bindings untouched — same
    // instance, not a replacement pointing at a dead catalog.
    ASSERT(reg.find("read_skill") == before);
    ASSERT(reg.find("list_skills") != nullptr);
    ASSERT(reg.find("write_skill") != nullptr);
}
