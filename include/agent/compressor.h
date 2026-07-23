// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_COMPRESSOR_H
#define AGENT_COMPRESSOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

class MemoryStore;
struct Memory;
struct Skill;

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

enum class Classification : std::uint8_t {
    core,
    context,
    prune
};

struct ArchiveEntry {
    std::string turn_range;
    std::string summary;
};

struct CompressedContext {
    struct Task {
        std::string name;
        std::string status;
        std::string goal;
        std::vector<std::string> decisions;
        std::vector<std::string> done;
        std::vector<std::string> pending;
    };

    std::vector<Task> tasks;
    std::vector<ArchiveEntry> archive;
    json facts;
    int version = 1;
};

struct CompressionBudget {
    double core     = 0.30;
    double archive  = 0.15;
    double headroom = 0.50;
};

struct CompressionResult {
    size_t messages_before = 0;
    size_t messages_after = 0;
    size_t tokens_before = 0;
    size_t tokens_after = 0;
    size_t core_count = 0;
    size_t context_count = 0;
    size_t prune_count = 0;
};

struct CompressionConfig {
    double threshold        = 0.75;
    int    min_turns        = 10;
    int    cooldown_turns   = 20;
    size_t summary_max_tokens = 200;
    CompressionBudget budget;
};

// One classified span in the LLM response.
struct ClassifiedSegment {
    size_t turn_start = 0;
    size_t turn_end = 0;
    Classification tag = Classification::context;
    std::string summary;
};

// One memory or skill operation from the LLM.
struct KnowledgeOp {
    std::string content;
    std::vector<std::string> tags;
    std::string action;             // "upsert" or "deprecate"
    std::string trigger_phrase;     // only for skills
};

// Structured result of parsing the LLM compression response.
struct CompressionResponse {
    std::vector<ClassifiedSegment> segments;
    std::vector<KnowledgeOp> memory_ops;
    std::vector<KnowledgeOp> skill_ops;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const std::vector<Message>& history,
                                  const Config& agent_cfg) const = 0;
    virtual void set_last_compress_turn(size_t turn) { (void)turn; }
    virtual bool is_within_cooldown(size_t current_turn) const {
        (void)current_turn; return false;
    }
};

class CompressionStrategy {
public:
    virtual ~CompressionStrategy() = default;
    virtual std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg,
        LLMClient& client) = 0;
};

// ---------------------------------------------------------------------------
// Pipeline modules (free functions)
// ---------------------------------------------------------------------------

// Collapse detected loops in history (modifies in place).
void collapse_loops(std::vector<Message>& history);

// Build the user message that asks the LLM to classify and compress.
Message build_compression_request(const std::vector<Message>& history);

// Parse the LLM's JSON response into a CompressionResponse.
CompressionResponse parse_compression_response(const std::string& json);

// Apply classification segments to history, returning compressed history.
std::vector<Message> apply_classification(
    const std::vector<Message>& history,
    const CompressionResponse& response);

// Apply memory/skill upsert/deprecate ops to a MemoryStore.
void apply_memory_ops(MemoryStore& store,
                      const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path);
void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path);

// Build a CompressionResult from before/after history and tags.
CompressionResult build_compression_result(
    const std::vector<Message>& before,
    const std::vector<Message>& after,
    const std::vector<Classification>& per_turn_tags);

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<CompressionStrategy> make_compressor(
    const CompressionConfig& cfg);

std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg);

CompressionConfig load_compression_config(const Config& cfg);

} // namespace agent

#endif // AGENT_COMPRESSOR_H
