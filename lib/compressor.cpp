
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/agent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
namespace agent {

// =========================================================================
// DefaultCompressionGate
// =========================================================================

// Re-arm a compaction that is still inside its cooldown once the prompt has
// grown by this share of the window (10%) or this many tokens, whichever is
// larger. The share keeps a large window from re-arming on noise; the floor
// keeps a small one from doing the same.
constexpr double kRearmGrowthShare = 0.10;
constexpr double kRearmFloorTokens = 1024.0;

class DefaultCompressionGate : public CompressionGate {
public:
    explicit DefaultCompressionGate(const CompressionConfig& cfg) : cfg_(cfg) {}

    bool should_compress(const Context& context, const Config& agent_cfg) const override {
        std::scoped_lock lk(mtx_);
        if (!threshold_exceeded(context, agent_cfg))
            return false;
        // A short history that is already nearly full must still compact: a
        // single turn with a few large results is under any message floor but
        // has nowhere left to grow.
        // Saturation bypasses the message floor, but not this: with only the
        // system prompt and the opening ask there is nothing a pass could
        // reduce, and two calls would buy a summary of one sentence.
        if (!has_reducible_history(context))
            return false;
        if (!sufficient_turns(context) &&
            conversation_utilisation(context) < kSaturationUtilisation)
            return false;
        // The cooldown is counted in user turns, and one tool-using turn can
        // refill the window several times: the gate re-arms once consumption has
        // grown by a share of the window instead of waiting for the next turn.
        if (cooldown_active(agent_cfg.user_turn_counter) && !regrown_since_attempt())
            return false;
        return true;
    }

    void set_last_compress_turn(size_t turn) override {
        std::scoped_lock lk(mtx_);
        last_compress_turn_ = turn;
        last_attempt_tokens_ = last_tokens_;
        attempted_ = true;
    }

    void set_threshold(double t) override {
        std::scoped_lock lk(mtx_);
        if (t > 0.0)
            cfg_.threshold = t;
    }

    void set_min_turns(int n) override {
        std::scoped_lock lk(mtx_);
        cfg_.min_turns = n;
    }

    // Last decision inputs, for the host's gate-fire debug log.
    void last_decision(double& tokens, double& budget, double& threshold) const override {
        std::scoped_lock lk(mtx_);
        tokens = last_tokens_;
        budget = last_budget_;
        threshold = last_threshold_;
    }

private:
    // Nearly full: the message floor stops mattering. Below the trigger, so a
    // saturated short session still compacts when the window is at its limit.
    static constexpr double kSaturationUtilisation = 0.90;

    double utilisation() const noexcept {
        return last_budget_ > 0 ? last_tokens_ / last_budget_ : 0.0;
    }

    bool threshold_exceeded(const Context& context, const Config& agent_cfg) const {
        // The budget is the window the model actually has: the active
        // model's probed context or an explicit config, clamped by anything
        // the server taught us via an overflow rejection. An unknown window
        // disables auto-compression — the gate never guesses (no arbitrary
        // fallback budget); /compress and the 400-overflow learner cover it.
        auto budget = static_cast<double>(agent_cfg.context_size);
        if (budget <= 0)
            return false;
        double tokens = agent_cfg.prompt_tokens_used > 0
                            ? static_cast<double>(agent_cfg.prompt_tokens_used)
                            : static_cast<double>(context.token_count());
        last_tokens_ = tokens;
        last_budget_ = budget;
        last_threshold_ = cfg_.threshold;
        return utilisation() >= cfg_.threshold;
    }

    bool sufficient_turns(const Context& context) const {
        return context.size() >= static_cast<size_t>(cfg_.min_turns);
    }

    // A pass can only shrink the conversation: it never touches the system
    // prompt, and the opening ask is the protected tail.
    static size_t conversation_messages(const Context& context) {
        size_t n = 0;
        for (const auto& m : context.get_all())
            if (m.role != "system")
                ++n;
        return n;
    }

    static bool has_reducible_history(const Context& context) {
        return conversation_messages(context) > 1;
    }

    // The share of the window the CONVERSATION occupies. The total includes the
    // immutable system prompt, which can dwarf a small window and would make
    // every session look saturated; what the floor guards is the conversation.
    double conversation_utilisation(const Context& context) const {
        if (last_budget_ <= 0)
            return 0.0;
        size_t tokens = 0;
        for (const auto& m : context.get_all())
            if (m.role != "system")
                tokens += message_tokens(m);
        return static_cast<double>(tokens) / last_budget_;
    }

    bool is_within_cooldown(size_t current_turn) const override {
        std::scoped_lock lk(mtx_);
        return cooldown_active(current_turn);
    }

    // last_compress_turn_ == 0 means "never compressed". The cooldown exists to
    // stop re-compressing the context a pass just produced — it must not block
    // the first compaction, because one tool-using turn is a single user turn
    // and may be the only chance the session gets. Callers hold mtx_.
    bool cooldown_active(size_t current_turn) const {
        if (last_compress_turn_ == 0)
            return false;
        return (current_turn - last_compress_turn_) < static_cast<size_t>(cfg_.cooldown_turns);
    }

    // Consumption, not elapsed requests: how much the prompt has grown since the
    // last attempt, as a share of the window (with a floor so a tiny window does
    // not re-arm on noise). A compaction that could not reduce leaves the size
    // unchanged, so this cannot spin.
    bool regrown_since_attempt() const {
        // Before the first attempt there is nothing to grow from: the cooldown
        // special case for a freshly restored session must stand.
        if (!attempted_ || last_budget_ <= 0)
            return false;
        const double need = std::max(kRearmFloorTokens, last_budget_ * kRearmGrowthShare);
        return last_tokens_ - last_attempt_tokens_ >= need;
    }

    CompressionConfig cfg_;
    mutable std::mutex mtx_;
    mutable size_t last_compress_turn_ = 0;
    mutable double last_tokens_ = 0;
    mutable double last_attempt_tokens_ = 0;
    mutable bool attempted_ = false;
    mutable double last_budget_ = 0;
    mutable double last_threshold_ = 0;
};

namespace {

// True when the history already carries a compressed-context message from a
// previous compression (re-compression updates its archive).
bool has_compressed_context(const std::vector<Message>& msgs) {
    for (const auto& m : msgs)
        if (m.content.compare(0, sizeof(kCompressedContextPrefix) - 1, kCompressedContextPrefix) ==
            0)
            return true;
    return false;
}

// The live request prefix, replayed: head blocks after the system prompt, then
// the conversation, then tail blocks. This is the order the host's
// insert_prompt_blocks produces, so the server's cached prefix is reused
// instead of re-prefilled. The instruction is appended by the caller.
std::vector<Message> with_request_prefix(const std::vector<Message>& history,
                                         const CompressionRequestPrefix& prefix) {
    std::vector<Message> out;
    out.reserve(history.size() + prefix.head_blocks.size() + prefix.tail_blocks.size());
    auto body = history.begin();
    if (!history.empty() && history.front().role == "system") {
        out.push_back(history.front());
        ++body;
    }
    out.insert(out.end(), prefix.head_blocks.begin(), prefix.head_blocks.end());
    out.insert(out.end(), body, history.end());
    out.insert(out.end(), prefix.tail_blocks.begin(), prefix.tail_blocks.end());
    return out;
}

// The free reduction pass: collapse loops, prune bulky tool output, drop
// reasoning text. It runs AFTER classification, on the output, so it cannot
// change the history the classifier saw (or the prefix the server cached).
// Reasoning is never sent back to the server (the dialect emits only content +
// tool_calls); in thinking models it is by far the largest token class, so
// dropping it is the single biggest reduction and loses nothing the model needs.
void collapse_and_prune(std::vector<Message>& copy, const CompressionConfig& cfg,
                        CompressionObserver* observer) {
    const size_t pre_loop = copy.size();
    collapse_loops(copy);
    if (observer) {
        const size_t removed = pre_loop - copy.size();
        if (removed > 0)
            observer->on_loop_collapse(removed);
        observer->on_progress(estimate_tokens(copy), copy.size());
    }
    prune_tool_io(copy, &cfg);
    for (auto& m : copy)
        m.reasoning.clear();
    if (observer)
        observer->on_progress(estimate_tokens(copy), copy.size());
}

enum class ClassifyOutcome { Ok, LlmFailed, Unparseable };

// Step 2: classify turns. The request is the LIVE prefix plus one instruction,
// so the server extends its cache instead of re-prefilling the conversation.
// On failure `cr.error` is set and reported to the observer; the caller returns
// the untouched original history.
ClassifyOutcome run_classify(const std::vector<Message>& history, LLMClient& client,
                             const CompressionRequestPrefix& prefix, CompressionObserver* observer,
                             CompressionResponse& cr, Message& class_reply) {
    auto request = with_request_prefix(history, prefix);
    request.push_back(build_classify_request(has_compressed_context(history), history));

    if (observer)
        observer->on_llm_request_sent();
    try {
        Stats stats;
        class_reply = client.chat(request, prefix.tools, &stats);
        if (observer)
            observer->on_llm_usage(stats);
    } catch (const std::exception& e) {
        cr.error = std::string("LLM call failed: ") + e.what();
        if (observer)
            observer->on_error(cr.error);
        return ClassifyOutcome::LlmFailed;
    }

    cr = parse_compression_response(class_reply.content);
    if (cr.segments.empty()) {
        cr.error = "unparseable compression response";
        if (observer)
            observer->on_error(cr.error);
        return ClassifyOutcome::Unparseable;
    }
    if (observer)
        observer->on_parse_result(cr);
    return ClassifyOutcome::Ok;
}

// Step 3: extract memories/skills (the second LLM call). The request replays the
// classify request (same live prefix, same instruction) plus the classify reply,
// so the two calls share a KV prefix — the extract step says "Review the
// classification above". Extraction failure is non-fatal: the classification
// result stands (spec CP-08: degraded but safe).
void run_extract(const std::vector<Message>& classify_prefix, const Message& class_reply,
                 const CompressionRequestPrefix& prefix, LLMClient& client,
                 CompressionObserver* observer, CompressionResponse& cr) {
    auto request = with_request_prefix(classify_prefix, prefix);
    request.push_back(
        build_classify_request(has_compressed_context(classify_prefix), classify_prefix));
    Message stored_reply;
    stored_reply.role = "assistant";
    stored_reply.content = class_reply.content;
    request.push_back(std::move(stored_reply));
    request.push_back(build_extract_request());

    if (observer)
        observer->on_llm_request_sent();
    Message ext_reply;
    try {
        Stats stats;
        ext_reply = client.chat(request, prefix.tools, &stats);
        if (observer)
            observer->on_llm_usage(stats);
    } catch (const std::exception&) {
        return; // non-fatal: keep the classification result
    }

    // Merge extraction ops into the classification response so the caller
    // receives segments AND ops in one object.
    CompressionResponse er = parse_compression_response(ext_reply.content);
    cr.memory_ops = std::move(er.memory_ops);
    cr.skill_ops = std::move(er.skill_ops);
    if (er.brief)
        cr.brief = std::move(er.brief);
}

} // namespace

// =========================================================================
// CompressionPipeline  —  orchestrates the full LLM-based compression
// =========================================================================

class CompressionPipeline : public CompressionStrategy {
public:
    std::vector<Message> compress(const Context& context, const CompressionConfig& cfg,
                                  LLMClient& client, CompressionObserver* observer,
                                  CompressionResponse* response_out,
                                  const CompressionRequestPrefix& prefix) override {
        // Pristine snapshot: the spec's atomicity invariant — any classify
        // failure returns the ORIGINAL history untouched. The context is only
        // ever read, and the classifier sees exactly that snapshot (plus the
        // live request prefix), so its indices stay aligned with apply.
        auto msgs = context.get_all();
        std::vector<Message> original(msgs.begin(), msgs.end());

        if (observer)
            observer->on_compress_start(original.size(), estimate_tokens(original));

        CompressionResponse cr;

        // Nothing older than the protected tail AND something inside it to
        // digest: the classifier has nothing to tag, so the turn's only relief
        // is its own older evidence, and that is a deterministic pass (D11).
        // Paying classify + extract to summarise one turn costs three
        // context-sized prefills to do what pruning does for one. When there is
        // nothing to digest the normal pipeline runs, so a manual compress still
        // reports the classifier's honest "no reduction".
        if (tail_is_whole_conversation(original, cfg)) {
            std::vector<Message> trimmed = original;
            const std::size_t replaced = prune_in_turn_results(trimmed, &cfg);
            if (replaced > 0 && estimate_tokens(trimmed) < estimate_tokens(original)) {
                if (observer) {
                    observer->on_apply_result({});
                    observer->on_progress(estimate_tokens(trimmed), trimmed.size());
                }
                if (response_out)
                    *response_out = std::move(cr);
                return trimmed;
            }
        }

        Message class_reply;
        if (run_classify(original, client, prefix, observer, cr, class_reply) !=
            ClassifyOutcome::Ok) {
            if (response_out)
                *response_out = std::move(cr);
            return original;
        }

        run_extract(original, class_reply, prefix, client, observer, cr);

        // Assemble, then reduce. The free pass runs on the OUTPUT so the
        // classifier sees the full tool evidence it needs to summarise, and the
        // live KV prefix it was sent against stays intact.
        std::vector<Message> compressed = apply_classification(original, cr, &cfg);
        collapse_and_prune(compressed, cfg, observer);

        // Enforce the post-compression target budget (context_size * target_pct
        // / 100): archive older core messages until the output fits the target.
        compressed = enforce_target_budget(std::move(compressed),
                                           static_cast<size_t>(cfg.context_size), cfg);
        // Enforce headroom: leave at least 25% free (final safety net; normally
        // already satisfied by the target budget). The protected tail is never
        // touched, so the active task survives even when it alone exceeds it.
        compressed =
            enforce_headroom(std::move(compressed), static_cast<size_t>(cfg.context_size), cfg);

        // Invariant 1: a compaction that does not reduce the history must not
        // replace it. Appending a summary to an identical list would cost tokens
        // and change the prefix for nothing. This is the honest outcome for a
        // single-turn session, whose whole turn is the protected tail.
        if (estimate_tokens(compressed) >= estimate_tokens(original)) {
            if (response_out)
                *response_out = std::move(cr);
            return original;
        }

        if (observer) {
            observer->on_apply_result({});
            // The working set just shrank to its final size — surface it so the
            // host's gauge drops before the swap completes.
            observer->on_progress(estimate_tokens(compressed), compressed.size());
        }
        if (response_out)
            *response_out = std::move(cr);
        return compressed;
    }
};

std::unique_ptr<CompressionStrategy> make_compressor() {
    return std::make_unique<CompressionPipeline>();
}

std::unique_ptr<CompressionGate> make_compression_gate(const CompressionConfig& cfg) {
    return std::make_unique<DefaultCompressionGate>(cfg);
}

CompressionConfig load_compression_config(const Config& cfg) {
    CompressionConfig cc;
    if (cfg.compression_threshold_explicit)
        cc.threshold = cfg.compression_threshold;
    if (cfg.compression_min_turns_explicit)
        cc.min_turns = cfg.compression_min_turns;
    if (cfg.compression_cooldown_turns_explicit)
        cc.cooldown_turns = cfg.compression_cooldown_turns;
    if (cfg.compression_target_pct_explicit)
        cc.target_pct = cfg.compression_target_pct;
    if (cfg.compression_keep_last_prompts_explicit)
        cc.keep_last_prompts = cfg.compression_keep_last_prompts;
    cc.context_size = cfg.context_size;
    return cc;
}

} // namespace agent