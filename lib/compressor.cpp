// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <unordered_set>

namespace agent {

// =========================================================================
// TreeShaker
// =========================================================================

namespace {

// Heuristic helpers used by TreeShaker.

bool is_decision_or_preference(const Message& msg) {
    if (msg.role != "assistant") return false;
    static const std::unordered_set<std::string> cues{
        "decided", "decide", "choice", "prefer", "preference",
        "let's use", "I'll use", "going with", "chosen"
    };
    return std::any_of(cues.begin(), cues.end(), [&](const std::string& cue) {
        return msg.content.find(cue) != std::string::npos;
    });
}

bool is_stale_tool_result(const Message& msg, size_t idx,
                           const std::vector<Message>& history) {
    if (msg.role != "tool") return false;
    for (size_t j = idx + 1; j < history.size(); ++j) {
        if (history[j].role == "user") return true;
        if (history[j].role == "assistant") break;
    }
    return false;
}

bool is_superseded_attempt(const Message& msg, size_t idx,
                            const std::vector<Message>& history) {
    if (msg.role != "tool") return false;
    for (size_t j = idx + 1; j < history.size(); ++j) {
        if (history[j].role != "tool") continue;
        if (history[j].name == msg.name) return true;
    }
    return false;
}

bool is_diagnostic_output(const Message& msg) {
    if (msg.role != "tool") return false;
    if (msg.content.size() < 100) return false;
    static const std::unordered_set<std::string> diagnostic_tools{
        "read", "grep", "search", "glob", "diff"
    };
    return diagnostic_tools.count(msg.name) > 0;
}

size_t find_last_user_message(const std::vector<Message>& history) {
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
        if (history[static_cast<size_t>(i)].role == "user")
            return static_cast<size_t>(i);
    }
    return 0;
}

} // namespace

// -----------------------------------------------------------------------
// TreeShaker  --  classifies every turn in the conversation
// -----------------------------------------------------------------------

TreeShaker::TreeShaker() = default;

std::vector<Classification> TreeShaker::classify(
    const std::vector<Message>& history) const {
    if (history.empty()) return {};

    std::vector<Classification> tags;
    tags.reserve(history.size());

    size_t active_root = find_last_user_message(history);

    for (size_t i = 0; i < history.size(); ++i) {
        const auto& msg = history[i];

        Classification tag = Classification::context;
        if (i >= active_root || is_decision_or_preference(msg)) {
            tag = Classification::core;
        } else if (is_diagnostic_output(msg) || is_superseded_attempt(msg, i, history)) {
            tag = Classification::prune;
        } else if (msg.role == "assistant" && !msg.reasoning.empty()) {
            tag = Classification::context;
        }
        tags.push_back(tag);
    }

    return tags;
}

// =========================================================================
// DefaultCompressionGate
// =========================================================================

class DefaultCompressionGate : public CompressionGate {
public:
    explicit DefaultCompressionGate(const CompressionConfig& cfg);
    bool should_compress(const std::vector<Message>& history,
                          const Config& agent_cfg) const override;
    void set_last_compress_turn(size_t turn) override;
    bool is_within_cooldown(size_t current_turn) const override;
private:
    bool threshold_exceeded(const std::vector<Message>& history,
                             const Config& agent_cfg) const;
    bool sufficient_turns(const std::vector<Message>& history) const;
    CompressionConfig cfg_;
    size_t last_compress_turn_ = 0;
};

DefaultCompressionGate::DefaultCompressionGate(const CompressionConfig& cfg)
    : cfg_(cfg) {}

bool DefaultCompressionGate::should_compress(
    const std::vector<Message>& history,
    const Config& agent_cfg) const {
    if (!threshold_exceeded(history, agent_cfg)) return false;
    if (!sufficient_turns(history)) return false;
    return true;
}

bool DefaultCompressionGate::threshold_exceeded(
    const std::vector<Message>& history,
    const Config& agent_cfg) const {
    if (agent_cfg.context_size <= 0) return false;
    size_t total_chars = 0;
    for (const auto& msg : history)
        total_chars += msg.content.size() + msg.reasoning.size();
    double utilisation = (static_cast<double>(total_chars) / 4.0)
                         / static_cast<double>(agent_cfg.context_size);
    return utilisation >= cfg_.threshold;
}

bool DefaultCompressionGate::sufficient_turns(
    const std::vector<Message>& history) const {
    return history.size() >= static_cast<size_t>(cfg_.min_turns);
}

void DefaultCompressionGate::set_last_compress_turn(size_t turn) {
    last_compress_turn_ = turn;
}

bool DefaultCompressionGate::is_within_cooldown(size_t current_turn) const {
    if (last_compress_turn_ == 0) return false;
    return (current_turn - last_compress_turn_)
           < static_cast<size_t>(cfg_.cooldown_turns);
}

// =========================================================================
// StructuredCompressor  --  assembles a CompressedContext from classified turns
// =========================================================================

class StructuredCompressor {
public:
    // Build a CompressedContext from the history and its classification tags.
    // Returns a message array suitable for the LLM prompt: core turns
    // verbatim + a synthetic assistant message containing the compressed
    // context as JSON.
    std::vector<Message> compress(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags,
        const CompressionConfig&) const {
        if (history.empty() || tags.empty()) return history;

        std::vector<Message> core;
        std::vector<size_t> archive_indices;

        for (size_t i = 0; i < history.size(); ++i) {
            switch (tags[i]) {
                case Classification::core:
                    core.push_back(history[i]);
                    break;
                case Classification::context:
                    archive_indices.push_back(i);
                    break;
                case Classification::prune:
                    break;  // dropped entirely
            }
        }

        // Build the JSON compressed context block
        json ctx;
        ctx["type"] = "compressed_context";
        ctx["version"] = 1;

        // Archive entries: summarise context-tagged segments
        json archive = json::array();
        if (!archive_indices.empty()) {
            size_t seg_start = archive_indices[0];
            size_t seg_end = archive_indices[0];
            for (size_t k = 1; k < archive_indices.size(); ++k) {
                if (archive_indices[k] == archive_indices[k - 1] + 1) {
                    seg_end = archive_indices[k];
                } else {
                    json entry;
                    entry["turns"] = std::to_string(seg_start) + "-"
                                     + std::to_string(seg_end);
                    entry["summary"] = "(compressed)";
                    archive.push_back(entry);
                    seg_start = archive_indices[k];
                    seg_end = archive_indices[k];
                }
            }
            json entry;
            entry["turns"] = std::to_string(seg_start) + "-"
                             + std::to_string(seg_end);
            entry["summary"] = "(compressed)";
            archive.push_back(entry);
        }
        ctx["archive"] = archive;

        // Facts: extract from core turns (decisions, preferences)
        json facts = json::object();
        for (const auto& msg : core) {
            if (msg.role == "user") {
                // Store user's goal statement
                if (msg.content.size() < 200)
                    facts["last_goal"] = msg.content;
            }
        }
        ctx["facts"] = facts;

        // Core verbatim turns + one compressed context message
        Message compressed_msg;
        compressed_msg.role = "system";
        compressed_msg.content = "Compressed conversation context:\n"
                                 + ctx.dump(2);
        core.push_back(compressed_msg);

        return core;
    }
};

// =========================================================================
// CompressionPipeline  --  sequences TreeShaker + StructuredCompressor
// =========================================================================

class CompressionPipeline : public CompressionStrategy {
public:
    CompressionPipeline()
        : shaker_(std::make_unique<TreeShaker>())
        , compressor_(std::make_unique<StructuredCompressor>()) {}

    std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg) override {
        auto tags = shaker_->classify(history);
        return compressor_->compress(history, tags, cfg);
    }

private:
    std::unique_ptr<TreeShaker> shaker_;
    std::unique_ptr<StructuredCompressor> compressor_;
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
