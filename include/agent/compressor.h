// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_COMPRESSOR_H
#define AGENT_COMPRESSOR_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

struct AgentHooks;
class MemoryStore;
struct Memory;
struct Skill;
struct ExtractionItem;

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

struct CompressionResult {
    size_t messages_before = 0;
    size_t messages_after = 0;
    size_t tokens_before = 0;
    size_t tokens_after = 0;
    size_t core_count = 0;
    size_t context_count = 0;
    size_t prune_count = 0;
    std::string error;               // non-empty when compression failed
};

struct CompressionConfig {
    double threshold        = 0.50;
    int    min_turns        = 10;
    int    cooldown_turns   = 20;
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
    std::string name;                // human-readable label
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

// Observer notified of compression pipeline progress. All methods have
// default no-op implementations so consumers only override what they need.
struct CompressionObserver {
    virtual ~CompressionObserver() = default;
    virtual void on_compress_start(size_t /*msgs_before*/, size_t /*tokens_before*/) {}
    virtual void on_loop_collapse(size_t /*removed*/) {}
    virtual void on_llm_request_sent() {}
    virtual void on_llm_reply_received(long /*elapsed_ms*/) {}
    virtual void on_parse_result(const CompressionResponse& /*cr*/) {}
    virtual void on_apply_result(const CompressionResult& /*r*/) {}
    virtual void on_memory_ops_applied(size_t /*upsert*/, size_t /*deprecate*/) {}
    virtual void on_error(const std::string& /*msg*/) {}
    virtual void on_compress_done(const CompressionResult& /*r*/) {}
};

class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const std::vector<Message>& history,
                                  const Config& agent_cfg) const = 0;
    virtual void set_last_compress_turn(size_t turn) { (void)turn; }
    virtual bool is_within_cooldown(size_t current_turn) const {
        (void)current_turn; return false;
    }
    virtual void set_threshold(double t) { (void)t; }
    virtual void set_min_turns(int n) { (void)n; }
};

class CompressionStrategy {
public:
    virtual ~CompressionStrategy() = default;
    // Compress `history` and return the compressed message list.
    // When `response_out` is non-null it receives the parsed CompressionResponse
    // containing memory/skill ops identified by the LLM. This lets callers apply
    // knowledge operations after compression without re-parsing.
    virtual std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg,
        LLMClient& client,
        CompressionObserver* observer = nullptr,
        CompressionResponse* response_out = nullptr) = 0;
};

// ---------------------------------------------------------------------------
// Pipeline modules (free functions)
// ---------------------------------------------------------------------------

// Collapse detected loops in history (modifies in place).
void collapse_loops(std::vector<Message>& history);

// Build the user message that asks the LLM to classify and compress.
Message build_compression_request();

// Parse the LLM's JSON response into a CompressionResponse.
CompressionResponse parse_compression_response(const std::string& json);

// Apply classification segments to history, returning compressed history.
std::vector<Message> apply_classification(
    const std::vector<Message>& history,
    const CompressionResponse& response);

// Apply memory/skill upsert/deprecate ops to a MemoryStore.
// When `items` is non-null, it receives one ExtractionItem per op for UI reporting.
void apply_memory_ops(MemoryStore& store,
                      const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path,
                      std::vector<ExtractionItem>* items = nullptr);
void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path,
                     std::vector<ExtractionItem>* items = nullptr);

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<CompressionStrategy> make_compressor(
    const CompressionConfig& cfg);

std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg);

CompressionConfig load_compression_config(const Config& cfg);

// Bridges CompressionObserver to AgentHooks for status reporting.
// Constructed with the hooks and a CompressionResult reference to fill.
// The log() method writes timestamped status lines via hooks_.on_status.
class CompressionReporter : public CompressionObserver {
public:
    CompressionReporter(const AgentHooks& hooks, CompressionResult& result);
    void set_before(size_t msgs, size_t tokens);
    void on_compress_start(size_t msgs, size_t) override;
    void on_loop_collapse(size_t removed) override;
    void on_llm_request_sent() override;
    void on_llm_reply_received(long sec) override;
    void on_parse_result(const CompressionResponse& cr) override;
    void on_apply_result(const CompressionResult&) override;
    void on_memory_ops_applied(size_t up, size_t dep) override;
    void on_error(const std::string& msg) override;
    void on_compress_done(const CompressionResult& final) override;
private:
    const AgentHooks& hooks_;
    CompressionResult& r_;
    std::chrono::steady_clock::time_point t0_;
    size_t before_msgs_ = 0;
    void log(const std::string& msg);
};

} // namespace agent

#endif // AGENT_COMPRESSOR_H
