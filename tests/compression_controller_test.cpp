// The compression lifecycle, exercised without the agent loop. The controller
// owns the gate, the pipeline call, the statistics and the cooldown; the agent
// keeps the run scope, the rebuild and the hosting side effects. These tests
// pin that split on a plain Context and a scripted strategy.

#include <string>
#include <vector>

#include "agent/compression_controller.h"
#include "agent/compressor.h"
#include "agent/config.h"
#include "agent/context.h"
#include "agent/llm.h"
#include "fake_llm.h"
#include "tests/test_util.h"

using namespace agent;

namespace {

class ScriptedStrategy : public CompressionStrategy {
public:
    std::vector<Message> result;
    std::string error;
    CompressionResponse response;
    int calls = 0;
    const Context* seen = nullptr;
    CompressionObserver* observer_seen = nullptr;
    CompressionRequestPrefix prefix_seen;

    std::vector<Message> compress(const Context& context, const CompressionConfig&,
                                  LLMClient&, CompressionObserver* observer,
                                  CompressionResponse* response_out,
                                  const CompressionRequestPrefix& prefix) override {
        ++calls;
        seen = &context;
        observer_seen = observer;
        prefix_seen = prefix;
        if (observer)
            observer->on_compress_start(context.size(), context.token_count());
        if (response_out)
            *response_out = response;
        if (!error.empty()) {
            response_out->error = error;
            return {};
        }
        return result;
    }
};

class ScriptedGate : public CompressionGate {
public:
    bool decision = false;
    double tokens = 0;
    double budget = 0;
    double threshold = 0;
    size_t last_turn = 0;
    double threshold_set = -1;
    int min_turns_set = -1;
    bool cooldown_recorded = false;

    bool should_compress(const Context&, const Config&) const override { return decision; }
    void set_last_compress_turn(size_t turn) override {
        last_turn = turn;
        cooldown_recorded = true;
    }
    void set_threshold(double t) override { threshold_set = t; }
    void set_min_turns(int n) override { min_turns_set = n; }
    void last_decision(double& t, double& b, double& th) const override {
        t = tokens;
        b = budget;
        th = threshold;
    }
};

Message plain_message(const std::string& role, const std::string& content) {
    Message msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

// system + user + assistant: the smallest conversation the controller accepts.
Context three_messages() {
    Context ctx;
    ctx.push(plain_message("system", "SYSTEM PROMPT"));
    ctx.push(plain_message("user", "do the thing"));
    ctx.push(plain_message("assistant", "working on it"));
    return ctx;
}

template <class Container> size_t conversation_tokens(const Container& messages) {
    size_t n = 0;
    for (const auto& m : messages)
        if (m.role != "system")
            n += message_tokens(m);
    return n;
}

} // namespace

// Without a strategy there is nothing to run, and the result says so without
// pretending a pass started: the host keys its "no compressor configured"
// message off messages_before == 0.
TEST(compression_controller_reports_a_missing_strategy) {
    CompressionController controller(nullptr, std::make_unique<ScriptedGate>());
    Config cfg;
    Context ctx = three_messages();
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix});
    ASSERT_FALSE(outcome.ok);
    ASSERT_FALSE(outcome.attempted);
    ASSERT_EQ(outcome.result.error, std::string("no compressor configured"));
    ASSERT_EQ(outcome.result.messages_before, 0u);
    ASSERT_TRUE(outcome.messages.empty());
}

// A conversation that cannot be classified is refused before the pipeline, with
// the real message count so the caller can report it.
TEST(compression_controller_refuses_a_conversation_too_short_to_compress) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* raw = strategy.get();
    CompressionController controller(std::move(strategy), nullptr);
    Config cfg;
    Context ctx;
    ctx.push(plain_message("system", "SYSTEM PROMPT"));
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix});
    ASSERT_FALSE(outcome.ok);
    ASSERT_FALSE(outcome.attempted);
    ASSERT_EQ(outcome.result.error, std::string("conversation too short to compress"));
    ASSERT_EQ(outcome.result.messages_before, 1u);
    ASSERT_EQ(raw->calls, 0);
}

// The pipeline reads the live context and returns the replacement list; the
// live stack is never rebuilt by the controller. Statistics describe both
// sides of the pass.
TEST(compression_controller_runs_the_pipeline_on_an_immutable_snapshot) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* raw = strategy.get();
    raw->result = {plain_message("system", "COMPRESSED SYSTEM"),
                   plain_message("user", "COMPRESSED TAIL")};
    raw->response.segments = {{0, 1, Classification::core, ""},
                              {1, 2, Classification::prune, ""},
                              {2, 3, Classification::context, ""}};
    CompressionController controller(std::move(strategy), nullptr);

    Config cfg;
    Context ctx = three_messages();
    const size_t tokens_before = ctx.token_count();
    const auto before_messages = ctx.get_all();
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix, nullptr, 4});
    ASSERT(outcome.ok);
    ASSERT(outcome.attempted);
    ASSERT_EQ(raw->seen, &ctx);
    ASSERT_EQ(outcome.messages.size(), 2u);
    ASSERT_EQ(outcome.result.messages_before, 3u);
    ASSERT_EQ(outcome.result.messages_after, 2u);
    ASSERT_EQ(outcome.result.tokens_before, tokens_before);
    ASSERT_EQ(outcome.result.tokens_after, estimate_tokens(outcome.messages));
    ASSERT_EQ(outcome.result.payload_tokens_before, conversation_tokens(before_messages));
    ASSERT_EQ(outcome.result.core_count, 1u);
    ASSERT_EQ(outcome.result.prune_count, 1u);
    ASSERT_EQ(outcome.result.context_count, 1u);
    ASSERT_TRUE(outcome.result.error.empty());
    ASSERT_EQ(ctx.size(), 3u);
    ASSERT_EQ(ctx.token_count(), tokens_before);
}

// Spec invariant 7: a failed pipeline leaves the live context exactly as it
// was. The failure still reports the pre-pass size and tokens, and still counts
// against the cooldown — a failing classifier must not re-fire every turn.
TEST(compression_controller_keeps_the_context_when_the_pipeline_fails) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* raw = strategy.get();
    raw->error = "classifier exploded";
    auto gate = std::make_unique<ScriptedGate>();
    ScriptedGate* gate_raw = gate.get();
    CompressionController controller(std::move(strategy), std::move(gate));

    Config cfg;
    Context ctx = three_messages();
    const auto before_messages = ctx.get_all();
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix, nullptr, 11});
    ASSERT_FALSE(outcome.ok);
    ASSERT(outcome.attempted);
    ASSERT_EQ(outcome.result.error, std::string("classifier exploded"));
    ASSERT_EQ(outcome.result.messages_before, 3u);
    ASSERT_EQ(outcome.result.tokens_before, ctx.token_count());
    ASSERT_TRUE(outcome.messages.empty());
    ASSERT_EQ(ctx.size(), 3u);
    ASSERT_EQ(ctx.get_all().size(), before_messages.size());
    ASSERT_EQ(ctx.get_all().front().content, before_messages.front().content);
    ASSERT(gate_raw->cooldown_recorded);
    ASSERT_EQ(gate_raw->last_turn, 11u);
}

// The gate is asked before any work, and the numbers it decided on are
// available for the host's debug log and typed event.
TEST(compression_controller_answers_the_gate_decision) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    auto gate = std::make_unique<ScriptedGate>();
    ScriptedGate* raw = gate.get();
    raw->tokens = 700.0;
    raw->budget = 1000.0;
    raw->threshold = 0.7;
    CompressionController controller(std::move(strategy), std::move(gate));

    Config cfg;
    Context ctx = three_messages();
    ASSERT(controller.auto_enabled());
    ASSERT_FALSE(controller.should_compress(ctx, cfg));
    raw->decision = true;
    ASSERT(controller.should_compress(ctx, cfg));

    const auto decision = controller.last_decision();
    ASSERT_EQ(decision.tokens, 700.0);
    ASSERT_EQ(decision.budget, 1000.0);
    ASSERT_EQ(decision.threshold, 0.7);

    controller.set_threshold(0.55);
    controller.set_min_turns(6);
    ASSERT_EQ(raw->threshold_set, 0.55);
    ASSERT_EQ(raw->min_turns_set, 6);
}

// Both halves are required for automatic compression: a strategy without a
// gate still supports manual /compress, and a gate without a strategy can
// never run.
TEST(compression_controller_needs_both_halves_to_auto_compress) {
    Config cfg;
    Context ctx = three_messages();

    CompressionController manual_only(std::make_unique<ScriptedStrategy>(), nullptr);
    ASSERT_FALSE(manual_only.auto_enabled());
    ASSERT_FALSE(manual_only.should_compress(ctx, cfg));

    CompressionController gate_only(nullptr, std::make_unique<ScriptedGate>());
    ASSERT_FALSE(gate_only.auto_enabled());
    ASSERT_FALSE(gate_only.should_compress(ctx, cfg));
}

// The compressed-context message is the archive the pass wrote; it is reported
// separately so the host can distinguish live conversation from bookkeeping.
TEST(compression_controller_counts_the_archive_it_wrote) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* raw = strategy.get();
    Message archive = plain_message("user", std::string(kCompressedContextPrefix) + " summary");
    raw->result = {plain_message("system", "COMPRESSED SYSTEM"), archive};
    CompressionController controller(std::move(strategy), nullptr);

    Config cfg;
    Context ctx = three_messages();
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix});
    ASSERT(outcome.ok);
    ASSERT_EQ(outcome.result.archive_tokens, message_tokens(archive));
    ASSERT_EQ(outcome.result.payload_tokens_after,
              conversation_tokens(outcome.messages) + outcome.result.archive_tokens);
}

// The last run is remembered for the fork path, which copies it so both legs
// describe the same compression state.
TEST(compression_controller_remembers_the_last_run) {
    auto strategy = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* raw = strategy.get();
    raw->result = {plain_message("system", "COMPRESSED SYSTEM")};
    CompressionController controller(std::move(strategy), nullptr);

    Config cfg;
    Context ctx = three_messages();
    agent_test::FakeLLMClient client;
    CompressionRequestPrefix prefix;

    const auto outcome = controller.run({ctx, client, cfg, prefix});
    ASSERT(outcome.ok);
    ASSERT_EQ(controller.last().tokens_after, outcome.result.tokens_after);

    CompressionResult replacement;
    replacement.messages_before = 42;
    controller.set_last(replacement);
    ASSERT_EQ(controller.last().messages_before, 42u);
}
