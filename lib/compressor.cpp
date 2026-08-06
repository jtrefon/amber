
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/agent.h"

#include <chrono>
#include <cmath>
#include <string>
namespace agent {

// When the server never reported n_ctx and no explicit config set the window
// (context_size <= 0), the gate falls back to this conservative budget so
// compression still runs instead of being disabled entirely. A known window
// (probed or explicit) is always the budget.
constexpr size_t kFallbackContextBudget = 32000;

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

private:
    bool threshold_exceeded(const Context& context,
                             const Config& agent_cfg) const {
        // The budget is the REAL window — probed n_ctx or explicit config.
        // The fallback only engages when the window is genuinely unknown, so
        // the threshold stays an honest fraction of the context the model
        // actually has.
        double budget = agent_cfg.context_size > 0
            ? static_cast<double>(agent_cfg.context_size)
            : static_cast<double>(kFallbackContextBudget);
        double tokens = agent_cfg.prompt_tokens_used > 0
            ? static_cast<double>(agent_cfg.prompt_tokens_used)
            : static_cast<double>(context.token_count());
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
        // failure returns the ORIGINAL history untouched (no loop collapse,
        // no partial apply). A working copy carries the mutations.
        auto msgs = context.get_all();
        std::vector<Message> original(msgs.begin(), msgs.end());
        std::vector<Message> copy = original;

        if (observer) observer->on_compress_start(copy.size(), 0);

        // Step 1: collapse loops (C++ side, free, on the working copy).
        size_t pre_loop = copy.size();
        collapse_loops(copy);
        if (observer) {
            size_t removed = pre_loop - copy.size();
            if (removed > 0) observer->on_loop_collapse(removed);
        }

        // Step 2: classify turns — append to LIVE context so the LLM
        // extends the KV cache from the current conversation prefix.
        CompressionResponse cr;
        {
            Message class_req = build_classify_request();
            context.push(class_req);

            if (observer) observer->on_llm_request_sent();
            Message class_reply;
            try {
                // Build request from the LIVE context (cached prefix
                // + classify prompt at the tail → KV extension)
                auto live = context.get_all();
                std::vector<Message> req(live.begin(), live.end());
                class_reply = client.chat(req, {});
            } catch (const std::exception& e) {
                context.pop();
                cr.error = std::string("LLM call failed: ") + e.what();
                if (response_out) *response_out = std::move(cr);
                if (observer) observer->on_error(cr.error);
                return original;
            }
            context.pop();  // remove classify request, restore context

            // Parse classification
            cr = parse_compression_response(class_reply.content);
            if (cr.segments.empty()) {
                cr.error = "unparseable compression response";
                if (response_out) *response_out = std::move(cr);
                if (observer) observer->on_error(cr.error);
                return original;
            }

            if (observer) observer->on_parse_result(cr);
            copy = apply_classification(copy, cr);

            // Enforce headroom: leave at least 25% free.
            copy = enforce_headroom(std::move(copy),
                                    static_cast<size_t>(cfg.context_size));

            if (observer) observer->on_apply_result({});
        }

        // Step 3: extract memories/skills (second LLM call).
        // Both calls share the SAME original context so they extend the
        // KV cache from the same prefix — no full prefill between them.
        {
            Message ext_req = build_extract_request();
            context.push(ext_req);

            if (observer) observer->on_llm_request_sent();
            Message ext_reply;
            try {
                auto live = context.get_all();
                std::vector<Message> req(live.begin(), live.end());
                ext_reply = client.chat(req, {});
            } catch (const std::exception&) {
                // Extraction failure is non-fatal — classification result used
                // (spec CP-08: degraded but safe).
                context.pop();
                if (response_out) *response_out = std::move(cr);
                return copy;
            }
            context.pop();  // remove extract request

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
    cc.context_size = cfg.context_size;
    return cc;
}

} // namespace agent