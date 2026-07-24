// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"
#include "agent/experience.h"
#include "agent/agent.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace agent {

// =========================================================================
// DefaultCompressionGate
// =========================================================================

class DefaultCompressionGate : public CompressionGate {
public:
    explicit DefaultCompressionGate(const CompressionConfig& cfg)
        : cfg_(cfg) {}

    bool should_compress(const std::vector<Message>& history,
                          const Config& agent_cfg) const override {
        if (!threshold_exceeded(history, agent_cfg)) return false;
        if (!sufficient_turns(history)) return false;
        return true;
    }

    void set_last_compress_turn(size_t turn) override {
        last_compress_turn_ = turn;
    }

    bool is_within_cooldown(size_t current_turn) const override {
        if (last_compress_turn_ == 0) return false;
        return (current_turn - last_compress_turn_) <
               static_cast<size_t>(cfg_.cooldown_turns);
    }

private:
    bool threshold_exceeded(const std::vector<Message>& history,
                             const Config& agent_cfg) const {
        if (agent_cfg.context_size <= 0) return false;
        size_t total_chars = 0;
        for (const auto& msg : history)
            total_chars += msg.content.size() + msg.reasoning.size();
        double utilisation = (static_cast<double>(total_chars) / 4.0) /
                             static_cast<double>(agent_cfg.context_size);
        return utilisation >= cfg_.threshold;
    }

    bool sufficient_turns(const std::vector<Message>& history) const {
        return history.size() >= static_cast<size_t>(cfg_.min_turns);
    }

    CompressionConfig cfg_;
    mutable size_t last_compress_turn_ = 0;
};

// =========================================================================
// CompressionPipeline  —  orchestrates the full LLM-based compression
// =========================================================================

class CompressionPipeline : public CompressionStrategy {
public:
    std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg,
        LLMClient& client,
        CompressionObserver* observer,
        CompressionResponse* response_out) override {
        (void)cfg;

        if (observer) observer->on_compress_start(history.size(), 0);

        auto copy = history;
        size_t pre_loop = copy.size();
        collapse_loops(copy);
        if (observer) {
            size_t removed = pre_loop - copy.size();
            if (removed > 0) observer->on_loop_collapse(removed);
        }

        Message request = build_compression_request(copy);
        copy.push_back(request);

        if (observer) observer->on_llm_request_sent();
        Message reply;
        try {
            reply = client.chat(copy, {});
        } catch (const std::exception&) {
            if (observer) observer->on_error("LLM call failed");
            return history;
        }
        copy.pop_back();

        if (observer) observer->on_llm_reply_received(0);
        CompressionResponse cr = parse_compression_response(reply.content);
        if (cr.segments.empty()) {
            if (observer) observer->on_error("unparseable compression response");
            return copy;
        }

        if (observer) observer->on_parse_result(cr);
        auto result = apply_classification(copy, cr);
        if (observer) observer->on_apply_result({});

        if (response_out) *response_out = std::move(cr);
        if (observer) observer->on_compress_done({});
        return result;
    }
};

// =========================================================================
// Factory functions
// =========================================================================

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
    return cc;
}

// =========================================================================
// CompressionReporter — bridges CompressionObserver to AgentHooks
// =========================================================================

CompressionReporter::CompressionReporter(const AgentHooks& hooks,
                                         CompressionResult& result)
    : hooks_(hooks), r_(result),
      t0_(std::chrono::steady_clock::now()), before_msgs_(0) {}

void CompressionReporter::set_before(size_t msgs, size_t tokens) {
    before_msgs_ = msgs; r_.messages_before = msgs; r_.tokens_before = tokens;
}

void CompressionReporter::on_compress_start(size_t msgs, size_t) {
    log("compress started (" + std::to_string(msgs) + " msgs)");
}

void CompressionReporter::on_loop_collapse(size_t removed) {
    log("loop collapse: removed " + std::to_string(removed) + " messages");
}

void CompressionReporter::on_llm_request_sent() { log("LLM request sent..."); }

void CompressionReporter::on_llm_reply_received(long sec) {
    log("LLM replied (" + std::to_string(sec) + "s)");
}

void CompressionReporter::on_parse_result(const CompressionResponse& cr) {
    log("parsed " + std::to_string(cr.segments.size()) + " spans, "
        + std::to_string(cr.memory_ops.size()) + " memory ops, "
        + std::to_string(cr.skill_ops.size()) + " skill ops");
}

void CompressionReporter::on_apply_result(const CompressionResult&) {
    log("apply complete");
}

void CompressionReporter::on_memory_ops_applied(size_t up, size_t dep) {
    log("store: " + std::to_string(up) + " upserts, "
        + std::to_string(dep) + " deprecations");
}

void CompressionReporter::on_error(const std::string& msg) {
    log("FAILED — " + msg);
}

void CompressionReporter::on_compress_done(const CompressionResult& final) {
    r_.messages_after = final.messages_after;
    r_.tokens_after = final.tokens_after;
    auto now = std::chrono::steady_clock::now();
    long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - t0_).count();
    log("finished in " + std::to_string(total_ms / 1000) + "s "
        + std::to_string(r_.messages_before) + " -> "
        + std::to_string(r_.messages_after) + " msgs");
}

void CompressionReporter::log(const std::string& msg) {
    auto now = std::chrono::steady_clock::now();
    long sec = std::chrono::duration_cast<std::chrono::seconds>(
        now - t0_).count();
    std::string line = "[+" + std::to_string(sec) + "s] " + msg + "\n";
    if (hooks_.on_status) hooks_.on_status(line);
}

} // namespace agent
