
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/agent.h"

#include <chrono>
#include <cmath>
#include <string>
namespace agent {

// =========================================================================
// DefaultCompressionGate
// =========================================================================

class DefaultCompressionGate : public CompressionGate {
public:
    explicit DefaultCompressionGate(const CompressionConfig& cfg)
        : cfg_(cfg) {}

    bool should_compress(const Context& context,
                         const Config& agent_cfg) const override {
        if (is_within_cooldown(agent_cfg.turn_counter)) return false;
        if (!threshold_exceeded(context, agent_cfg)) return false;
        if (!sufficient_turns(context)) return false;
        return true;
    }

    void set_last_compress_turn(size_t turn) override {
        last_compress_turn_ = turn;
    }

    void set_threshold(double t) override {
        if (t > 0.0) cfg_.threshold = t;
    }

    void set_min_turns(int n) override { cfg_.min_turns = n; }

    // Last decision inputs, for the host's gate-fire debug log.
    void last_decision(double& tokens, double& budget,
                       double& threshold) const override {
        tokens = last_tokens_;
        budget = last_budget_;
        threshold = last_threshold_;
    }

private:
    bool threshold_exceeded(const Context& context,
                            const Config& agent_cfg) const {
        // The budget is the window the model actually has: the active
        // model's probed context or an explicit config, clamped by anything
        // the server taught us via an overflow rejection. An unknown window
        // disables auto-compression — the gate never guesses (no arbitrary
        // fallback budget); /compress and the 400-overflow learner cover it.
        auto budget = static_cast<double>(agent_cfg.context_size);
        if (budget <= 0) return false;
        double tokens = agent_cfg.prompt_tokens_used > 0
            ? static_cast<double>(agent_cfg.prompt_tokens_used)
            : static_cast<double>(context.token_count());
        last_tokens_ = tokens;
        last_budget_ = budget;
        last_threshold_ = cfg_.threshold;
        double utilisation = tokens / budget;
        return utilisation >= cfg_.threshold;
    }

    bool sufficient_turns(const Context& context) const {
        return context.size() >= static_cast<size_t>(cfg_.min_turns);
    }

    bool is_within_cooldown(size_t current_turn) const override {
        // last_compress_turn_ == 0 means "never compressed": only the very
        // first turn of a freshly loaded session counts as cooldown (so a
        // restored large session is not compressed immediately); any later
        // turn is free to trigger the first compression. After the first
        // compression, normal cooldown applies.
        if (last_compress_turn_ == 0)
            return current_turn == 0 && cfg_.cooldown_turns > 0;
        return (current_turn - last_compress_turn_) <
               static_cast<size_t>(cfg_.cooldown_turns);
    }

    CompressionConfig cfg_;
    mutable size_t last_compress_turn_ = 0;
    mutable double last_tokens_ = 0;
    mutable double last_budget_ = 0;
    mutable double last_threshold_ = 0;
};

// =========================================================================
// CompressionReporter  —  bridges pipeline events to AgentHooks
// =========================================================================

CompressionReporter::CompressionReporter(const AgentHooks& hooks,
                                         CompressionResult& result,
                                         std::function<void()> progress)
    : hooks_(hooks), r_(result), progress_(std::move(progress)) {}

void CompressionReporter::set_before(size_t msgs, size_t tokens) {
    before_msgs_ = msgs;
    r_.messages_before = msgs;
    r_.tokens_before = tokens;
}

void CompressionReporter::on_compress_start(size_t msgs, size_t) {
    log("compressing " + std::to_string(msgs) + " messages...");
    t0_ = std::chrono::steady_clock::now();
    pump();
}

void CompressionReporter::on_loop_collapse(size_t removed) {
    log("loop collapse removed " + std::to_string(removed) + " messages");
    pump();
}

void CompressionReporter::on_progress(size_t tokens, size_t msgs) {
    // Emitted as the working set shrinks (collapse/prune/apply). Keep it a
    // compact status line so the host's context gauge can tick downward
    // without spamming the scrollback.
    log("  ... " + std::to_string(msgs) + " msgs, ~"
        + std::to_string(tokens) + " tok remain");
    pump();
}

void CompressionReporter::on_llm_request_sent() {
    log("sending LLM request...");
    pump();
}

void CompressionReporter::on_llm_reply_received(long sec) {
    log("LLM replied in " + std::to_string(sec) + "s");
    pump();
}

void CompressionReporter::on_parse_result(const CompressionResponse& cr) {
    size_t core = 0, prune = 0, ctx = 0;
    for (const auto& seg : cr.segments) {
        switch (seg.tag) {
            case Classification::core:    ++core; break;
            case Classification::prune:   ++prune; break;
            case Classification::context: ++ctx; break;
        }
    }
    std::string seg_info = std::to_string(cr.segments.size()) + " segments"
        + " (" + std::to_string(core) + " core, "
        + std::to_string(prune) + " prune, "
        + std::to_string(ctx) + " archive)";
    log("parsed " + seg_info
        + ", " + std::to_string(cr.memory_ops.size()) + " mem ops"
        + ", " + std::to_string(cr.skill_ops.size()) + " skill ops");

    // Show each memory/skill op that was extracted
    for (const auto& m : cr.memory_ops)
        log("  [mem] " + m.action + " \"" + m.name + "\""
            + (m.content.empty() ? "" : ": " + m.content.substr(0, 100)));
    for (const auto& s : cr.skill_ops)
        log("  [skill] " + s.action + " \"" + s.name + "\""
            + (s.content.empty() ? "" : ": " + s.content.substr(0, 100)));
    pump();
}

void CompressionReporter::on_apply_result(const CompressionResult&) {
    log("applied classification");
    pump();
}

void CompressionReporter::on_error(const std::string& msg) {
    log("error: " + msg);
    pump();
}

void CompressionReporter::on_compress_done(const CompressionResult& final) {
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0_).count();
    r_.messages_after = final.messages_after;
    r_.tokens_after = final.tokens_after;
    std::string detail;
    if (r_.messages_before > r_.messages_after)
        detail += std::to_string(r_.messages_before - r_.messages_after) + " msgs removed";
    if (r_.tokens_before > r_.tokens_after) {
        if (!detail.empty()) detail += ", ";
        detail += std::to_string(r_.tokens_before) + " → "
                  + std::to_string(r_.tokens_after) + " tok";
    }
    if (detail.empty())
        detail = std::to_string(r_.tokens_after) + " tok";
    log("done in " + std::to_string(elapsed) + "s — " + detail);
}

void CompressionReporter::log(const std::string& msg) {
    if (hooks_.on_status) hooks_.on_status(msg);
}

void CompressionReporter::pump() {
    if (progress_) progress_();
}

namespace {

// True when the history already carries a compressed-context message from a
// previous compression (re-compression updates its archive).
bool has_compressed_context(const std::vector<Message>& msgs) {
    for (const auto& m : msgs)
        if (m.content.compare(0, sizeof(kCompressedContextPrefix) - 1,
                              kCompressedContextPrefix) == 0)
            return true;
    return false;
}

// Append one message to a request vector (classify/extract tails).
std::vector<Message> append_message(std::vector<Message> base,
                                    Message m) {
    base.push_back(std::move(m));
    return base;
}

} // namespace

// =========================================================================
// CompressionPipeline  —  orchestrates the full LLM-based compression
// =========================================================================

class CompressionPipeline : public CompressionStrategy {
public:
    std::vector<Message> compress(
        Context& context,
        const CompressionConfig& cfg,
        LLMClient& client,
        CompressionObserver* observer,
        CompressionResponse* response_out) override {

        // Pristine snapshot: the spec's atomicity invariant — any classify
        // failure returns the ORIGINAL history untouched. The context is
        // only ever read; all mutations happen on the working copy, which
        // is also the exact message list the classifier sees — segment
        // indices stay aligned between request and apply.
        auto msgs = context.get_all();
        std::vector<Message> original(msgs.begin(), msgs.end());
        std::vector<Message> copy = original;

        if (observer)
            observer->on_compress_start(copy.size(), estimate_tokens(copy));

        // Step 1: collapse loops + Phase-0 tool-output pruning (C++ side,
        // free, on the working copy). Both run BEFORE the classifier so the
        // classify request and the apply pass consume the SAME message
        // list — no index shift between them.
        size_t pre_loop = copy.size();
        collapse_loops(copy);
        if (observer) {
            size_t removed = pre_loop - copy.size();
            if (removed > 0) observer->on_loop_collapse(removed);
            observer->on_progress(estimate_tokens(copy), copy.size());
        }
        prune_tool_io(copy, &cfg);
        if (observer)
            observer->on_progress(estimate_tokens(copy), copy.size());

        // The classify prefix is the post-collapse history (the message list
        // the classifier sees). The extract request replays this SAME prefix
        // plus the classify prompt + response, so the second LLM call shares
        // the first's KV cache (no full prefill between the compression
        // calls). Keep it aside — apply_classification below must not clobber
        // the prefix the extract step replays.
        std::vector<Message> classify_prefix = copy;

        // Step 2: classify turns — the request is assembled from the
        // working copy, so the LLM sees exactly what apply will consume.
        CompressionResponse cr;
        Message class_reply;
        {
            Message class_req =
                build_classify_request(has_compressed_context(copy));
            auto request = append_message(copy, class_req);

            if (observer) observer->on_llm_request_sent();
            try {
                class_reply = client.chat(request, {});
            } catch (const std::exception& e) {
                cr.error = std::string("LLM call failed: ") + e.what();
                if (observer) observer->on_error(cr.error);
                if (response_out) *response_out = std::move(cr);
                return original;
            }

            // Parse classification
            cr = parse_compression_response(class_reply.content);
            if (cr.segments.empty()) {
                cr.error = "unparseable compression response";
                if (observer) observer->on_error(cr.error);
                if (response_out) *response_out = std::move(cr);
                return original;
            }

            if (observer) observer->on_parse_result(cr);
            copy = apply_classification(copy, cr, &cfg);

            // Enforce the post-compression target budget (context_size *
            // target_pct / 100): archive older core messages until the output
            // fits the target — the real lever for aggressive compression.
            copy = enforce_target_budget(std::move(copy),
                                         static_cast<size_t>(cfg.context_size),
                                         cfg);

            // Enforce headroom: leave at least 25% free (final safety net;
            // normally already satisfied by the target budget).
            copy = enforce_headroom(std::move(copy),
                                    static_cast<size_t>(cfg.context_size));

            if (observer) {
                observer->on_apply_result({});
                // The working set just shrank to its final size — surface it
                // so the host's gauge drops before the swap completes.
                observer->on_progress(estimate_tokens(copy), copy.size());
            }
        }

        // Step 3: extract memories/skills (second LLM call). The request
        // replays the classify pair on the ORIGINAL (post-collapse) prefix so
        // the two LLM calls share a KV prefix — the extract step says "Review
        // the classification above", and the classify prompt + its response
        // are replayed before the extract prompt.
        {
            Message class_req =
                build_classify_request(has_compressed_context(classify_prefix));
            Message ext_req = build_extract_request();
            auto request = append_message(classify_prefix, class_req);
            Message stored_reply;
            stored_reply.role = "assistant";
            stored_reply.content = class_reply.content;
            request = append_message(std::move(request), std::move(stored_reply));
            request = append_message(std::move(request), ext_req);

            if (observer) observer->on_llm_request_sent();
            Message ext_reply;
            try {
                ext_reply = client.chat(request, {});
            } catch (const std::exception&) {
                // Extraction failure is non-fatal — classification result used
                // (spec CP-08: degraded but safe).
                if (response_out) *response_out = std::move(cr);
                return copy;
            }

            // Merge extraction ops into the classification response so the
            // caller receives segments AND ops in one object.
            CompressionResponse er = parse_compression_response(ext_reply.content);
            cr.memory_ops = std::move(er.memory_ops);
            cr.skill_ops = std::move(er.skill_ops);
        }

        if (response_out) *response_out = std::move(cr);
        return copy;
    }
};

std::unique_ptr<CompressionStrategy> make_compressor(
    const CompressionConfig& cfg) {
    (void)cfg;
    return std::make_unique<CompressionPipeline>();
}

std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg) {
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