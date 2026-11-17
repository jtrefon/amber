#include "agent/compression_controller.h"

#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/llm.h"

#include <utility>

namespace agent {

namespace {

// Tokens outside the immutable system prompt: the part compaction can change.
// Including the prompt in a compaction ratio hides the effect, because most of
// a small session's tokens are the prompt itself.
template <class Container> std::size_t conversation_token_count(const Container& msgs) {
    std::size_t n = 0;
    for (const auto& m : msgs)
        if (m.role != "system")
            n += message_tokens(m);
    return n;
}

void count_segments(const CompressionResponse& response, CompressionResult& result) {
    for (const auto& segment : response.segments) {
        switch (segment.tag) {
        case Classification::core:
            ++result.core_count;
            break;
        case Classification::context:
            ++result.context_count;
            break;
        case Classification::prune:
            ++result.prune_count;
            break;
        }
    }
}

// The compressed-context message is the bookkeeping the pass wrote; report it
// separately from the live conversation it sits in.
std::size_t archive_tokens(const std::vector<Message>& messages) {
    std::size_t n = 0;
    for (const auto& m : messages)
        if (m.content.rfind(kCompressedContextPrefix, 0) == 0)
            n += message_tokens(m);
    return n;
}

} // namespace

CompressionController::CompressionController(std::unique_ptr<CompressionStrategy> strategy,
                                             std::unique_ptr<CompressionGate> gate)
    : strategy_(std::move(strategy)), gate_(std::move(gate)) {}

CompressionController::~CompressionController() = default;
CompressionController::CompressionController(CompressionController&&) noexcept = default;
CompressionController& CompressionController::operator=(CompressionController&&) noexcept = default;

bool CompressionController::auto_enabled() const noexcept {
    return strategy_ != nullptr && gate_ != nullptr;
}

bool CompressionController::should_compress(const Context& context, const Config& cfg) const {
    return auto_enabled() && gate_->should_compress(context, cfg);
}

CompressionController::GateDecision CompressionController::last_decision() const {
    GateDecision decision;
    if (gate_)
        gate_->last_decision(decision.tokens, decision.budget, decision.threshold);
    return decision;
}

void CompressionController::set_threshold(double threshold) {
    if (gate_)
        gate_->set_threshold(threshold);
}

void CompressionController::set_min_turns(int turns) {
    if (gate_)
        gate_->set_min_turns(turns);
}

CompressionController::Outcome CompressionController::run(const Request& request) {
    Outcome outcome;
    // Every attempt is remembered, including a refusal: /compress reports the
    // same result it just handed back, and a fork copies it.
    auto finish = [this](Outcome result) {
        last_ = result.result;
        return result;
    };
    if (!strategy_) {
        outcome.result.error = "no compressor configured";
        return finish(std::move(outcome));
    }
    if (request.context.size() < 2) {
        outcome.result.messages_before = request.context.size();
        outcome.result.error = "conversation too short to compress";
        return finish(std::move(outcome));
    }
    outcome.attempted = true;

    // Snapshot BEFORE compression — immutable, never mutate the live stack.
    const auto before = request.context.get_all();
    const std::size_t msgs_before = before.size();
    const std::size_t tokens_before = request.context.token_count();
    outcome.result.payload_tokens_before = conversation_token_count(before);

    const CompressionConfig cfg = load_compression_config(request.cfg);
    outcome.messages = strategy_->compress(request.context, cfg, request.client, request.observer,
                                           &outcome.response, request.prefix);

    // Cooldown applies to the attempt, not just the success: a failing
    // classifier must not re-fire the pipeline on every following turn and burn
    // two LLM calls each time — the gate stays silent for the cooldown window
    // and retries later.
    if (gate_)
        gate_->set_last_compress_turn(request.turn);

    if (!outcome.response.error.empty()) {
        outcome.result.messages_before = msgs_before;
        outcome.result.tokens_before = tokens_before;
        outcome.result.error = outcome.response.error;
        outcome.messages.clear();
        return finish(std::move(outcome));
    }

    outcome.ok = true;
    outcome.result.messages_before = msgs_before;
    outcome.result.messages_after = outcome.messages.size();
    outcome.result.tokens_before = tokens_before;
    outcome.result.tokens_after = estimate_tokens(outcome.messages);
    count_segments(outcome.response, outcome.result);
    outcome.result.archive_tokens = archive_tokens(outcome.messages);
    outcome.result.payload_tokens_after =
        conversation_token_count(outcome.messages) + outcome.result.archive_tokens;
    return finish(std::move(outcome));
}

const CompressionResult& CompressionController::last() const noexcept {
    return last_;
}

void CompressionController::set_last(CompressionResult result) {
    last_ = std::move(result);
}

} // namespace agent
