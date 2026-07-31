
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"
#include "agent/agent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
namespace agent {

// When the server never reported n_ctx (context_size <= 0), the gate falls
// back to this conservative budget so compression still runs instead of
// being disabled entirely. Explicit config values and probed n_ctx win.
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

    void set_min_turns(int n) override {
        if (n > 0) cfg_.min_turns = n;
    }

private:
    bool threshold_exceeded(const Context& context,
                             const Config& agent_cfg) const {
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
        // When last_compress_turn_ is 0 AND current_turn is also 0 (fresh
        // restart / session load), treat it as within cooldown so the gate
        // doesn't fire immediately on the first turn after loading a large
        // session. Once compression has run once, set_last_compress_turn
        // updates this to a non-zero value and normal cooldown applies.
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
                                         CompressionResult& result)
    : hooks_(hooks), r_(result) {}

void CompressionReporter::set_before(size_t msgs, size_t tokens) {
    before_msgs_ = msgs;
    r_.messages_before = msgs;
    r_.tokens_before = tokens;
}

void CompressionReporter::on_compress_start(size_t msgs, size_t) {
    log("compressing " + std::to_string(msgs) + " messages...");
    t0_ = std::chrono::steady_clock::now();
}

void CompressionReporter::on_loop_collapse(size_t removed) {
    log("loop collapse removed " + std::to_string(removed) + " messages");
}

void CompressionReporter::on_llm_request_sent() {
    log("sending LLM request...");
}

void CompressionReporter::on_llm_reply_received(long sec) {
    log("LLM replied in " + std::to_string(sec) + "s");
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
}

void CompressionReporter::on_apply_result(const CompressionResult&) {
    log("applied classification");
}

void CompressionReporter::on_memory_ops_applied(size_t up, size_t dep) {
    log("memory store: " + std::to_string(up) + " upserted, "
        + std::to_string(dep) + " deprecated");
}

void CompressionReporter::on_error(const std::string& msg) {
    log("error: " + msg);
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

        // Snapshot the current messages for loop collapse and as the base
        // for apply_classification. We work with the live context for LLM
        // calls so the classify/extract prompts extend the KV cache instead
        // of forcing a full prefill.
        auto msgs = context.get_all();
        std::vector<Message> copy(msgs.begin(), msgs.end());

        if (observer) observer->on_compress_start(copy.size(), 0);

        // Step 1: collapse loops (C++ side, free, on the snapshot)
        size_t pre_loop = copy.size();
        collapse_loops(copy);
        if (observer) {
            size_t removed = pre_loop - copy.size();
            if (removed > 0) observer->on_loop_collapse(removed);
        }

        // Step 2: classify turns — append to LIVE context so the LLM
        // extends the KV cache from the current conversation prefix.
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
                if (observer) observer->on_error(std::string("LLM call failed: ") + e.what());
                return copy;
            }
            context.pop();  // remove classify request, restore context

            // Parse classification
            CompressionResponse cr = parse_compression_response(class_reply.content);
            if (cr.segments.empty()) {
                if (observer) observer->on_error("unparseable compression response");
                return copy;
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
                context.pop();
                // Build CompressedResponse from what we have
                CompressionResponse cr;
                // Re-parse from the compressed context (simplified: empty response)
                if (response_out) *response_out = std::move(cr);
                return copy;
            }
            context.pop();  // remove extract request

            // Parse extraction and merge into the compression response
            CompressionResponse er = parse_compression_response(ext_reply.content);
            CompressionResponse cr;
            if (!er.memory_ops.empty() || !er.skill_ops.empty()) {
                cr.memory_ops = std::move(er.memory_ops);
                cr.skill_ops = std::move(er.skill_ops);
            }
            if (response_out) *response_out = std::move(cr);
        }

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
    if (cfg.compression_threshold > 0.0)
        cc.threshold = cfg.compression_threshold;
    if (cfg.compression_min_turns > 0)
        cc.min_turns = cfg.compression_min_turns;
    if (cfg.compression_cooldown_turns > 0)
        cc.cooldown_turns = cfg.compression_cooldown_turns;
    cc.context_size = cfg.context_size;
    return cc;
}

} // namespace agent