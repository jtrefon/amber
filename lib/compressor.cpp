// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"
#include "agent/experience.h"

#include <algorithm>
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
        LLMClient& client) override {
        (void)cfg;

        // Step 1: mutable copy
        auto copy = history;

        // Step 2: collapse detected loops
        collapse_loops(copy);

        // Step 3: build and append compression request
        Message request = build_compression_request(copy);
        copy.push_back(request);

        // Step 4: call LLM (same system prompt, no tools)
        Message reply;
        try {
            reply = client.chat(copy, {});
        } catch (const std::exception&) {
            // LLM call failed — return original history
            return history;
        }

        // Step 5: remove the request from history (keep it from the LLM's
        // perspective but don't store it permanently)
        copy.pop_back();

        // Step 6: parse the LLM response
        CompressionResponse cr = parse_compression_response(reply.content);
        if (cr.segments.empty()) {
            // Parse failed or empty response — return copy unchanged
            return copy;
        }

        // Step 7: apply classification to produce compressed history
        return apply_classification(copy, cr);
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

} // namespace agent
