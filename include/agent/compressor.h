
#ifndef AGENT_COMPRESSOR_H
#define AGENT_COMPRESSOR_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

class Context;
struct AgentHooks;
class MemoryStore;
struct Memory;
struct Skill;
struct ExtractionItem;

// Single source of truth for the default compression threshold. The gate,
// load_compression_config, and the UIs all read this value.
inline constexpr double kDefaultCompressionThreshold = 0.70;

// Longest summary the pipeline accepts from the classifier; anything longer
// is truncated so a slop response cannot bloat the archive message.
inline constexpr std::size_t kMaxSummaryChars = 200;

// Placeholder that replaces bulky old tool outputs in Phase-0 pruning. The
// session log retains the original content.
inline constexpr char kToolOutputOmitted[] = "[tool output omitted \u2014 session log retains the original]";

// Prefix of the compressed-context message produced by apply_classification;
// enforce_headroom locates it by this prefix to append archive entries.
inline constexpr char kCompressedContextPrefix[] =
    "Compressed conversation context:";

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

enum class Classification : std::uint8_t {
    core,
    context,
    prune
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
    double threshold        = kDefaultCompressionThreshold;
    int    min_turns        = 10;
    int    cooldown_turns   = 20;
    int    context_size     = 0;       // filled from Config for headroom enforcement
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
    std::string error;               // non-empty when the pipeline failed;
                                     // callers must keep the context untouched
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
    virtual void on_error(const std::string& /*msg*/) {}
    virtual void on_compress_done(const CompressionResult& /*r*/) {}
    // Intermediate progress: fired as the pipeline moves through phases so a
    // host can render a live (decreasing) context gauge. tokens_remaining /
    // msgs_remaining are the current working estimate, which shrinks as
    // collapse/prune/apply drop messages.
    virtual void on_progress(size_t /*tokens_remaining*/,
                             size_t /*msgs_remaining*/) {}
};

class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const Context& context,
                                  const Config& agent_cfg) const = 0;
    virtual void set_last_compress_turn(size_t turn) { (void)turn; }
    virtual bool is_within_cooldown(size_t current_turn) const {
        (void)current_turn; return false;
    }
    virtual void set_threshold(double t) { (void)t; }
    virtual void set_min_turns(int n) { (void)n; }
    // Last decision inputs (tokens, window, threshold) for the host's
    // gate-fire debug log. Default no-op for custom gates.
    virtual void last_decision(double& /*tokens*/, double& /*budget*/,
                               double& /*threshold*/) const {}
};

class CompressionStrategy {
public:
    virtual ~CompressionStrategy() = default;
    // Compress the conversation in `context`. Pure: the context is only
    // READ — classify/extract requests are assembled from a working copy of
    // the snapshot, so a failed pipeline leaves the context untouched by
    // construction (spec invariant 7). The caller rebuilds the context from
    // the returned message list on success.
    // Returns the compressed message list.
    virtual std::vector<Message> compress(
        Context& context,
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

// Phase-0 cheap pass: replace bulky tool outputs (>200 chars) OUTSIDE the
// protected tail (everything from the second-to-last user message onward)
// with a short placeholder. No LLM call; returns the number of messages
// replaced. The session log retains the original content.
std::size_t prune_tool_io(std::vector<Message>& history);

// Repair tool_call/tool_result group splits left by classification: orphaned
// tool messages (no preceding assistant tool_calls) are removed, and
// assistant tool_calls whose results were pruned get a stub tool message
// injected so the message sequence stays API-valid.
void sanitize_tool_pairs(std::vector<Message>& history);

// Build a classification-only request (returns array of turn tags).
// This is the first step of the multi-step pipeline. When `update_previous`
// is true the conversation already carries a compressed-context message:
// the classifier is told to extend its archive instead of re-summarizing.
Message build_classify_request(bool update_previous = false);

// Build an extraction request (returns memories + skills).
// This is the second step, appended after the classification response.
Message build_extract_request();

// Parse the LLM's JSON response into a CompressionResponse.
CompressionResponse parse_compression_response(const std::string& json);

// Apply classification segments to history, returning compressed history.
std::vector<Message> apply_classification(
    const std::vector<Message>& history,
    const CompressionResponse& response);

// Apply memory/skill upsert/deprecate ops to a MemoryStore.
// When `items` is non-null, it receives one ExtractionItem per op for UI
// reporting. The store owns promotion thresholds.
void apply_memory_ops(MemoryStore& store,
                      const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path,
                      std::vector<ExtractionItem>* items = nullptr);
void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path,
                     std::vector<ExtractionItem>* items = nullptr);

// Enforce that the compressed context leaves at least 25% headroom.
// Drops oldest non-system messages, recording them as archive entries on
// the compressed-context message. The system prompt at index 0 is never
// modified. Returns the tightened vector (it may be shorter than input).
std::vector<Message> enforce_headroom(std::vector<Message> compressed,
                                       size_t context_size);

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<CompressionStrategy> make_compressor(
    const CompressionConfig& cfg);

std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg);

CompressionConfig load_compression_config(const Config& cfg);

// Bridges CompressionObserver to AgentHooks for status reporting.
// Constructed with the hooks, a CompressionResult reference to fill, and an
// optional progress callback invoked after every event so hosts can pump
// their event loop during long-running compression.
// The log() method writes timestamped status lines via hooks_.on_status.
class CompressionReporter : public CompressionObserver {
public:
    CompressionReporter(const AgentHooks& hooks, CompressionResult& result,
                        std::function<void()> progress = {});
    void set_before(size_t msgs, size_t tokens);
    void on_compress_start(size_t msgs, size_t) override;
    void on_loop_collapse(size_t removed) override;
    void on_llm_request_sent() override;
    void on_llm_reply_received(long sec) override;
    void on_parse_result(const CompressionResponse& cr) override;
    void on_apply_result(const CompressionResult&) override;
    void on_error(const std::string& msg) override;
    void on_compress_done(const CompressionResult& final) override;
    void on_progress(size_t tokens, size_t msgs) override;
private:
    const AgentHooks& hooks_;
    CompressionResult& r_;
    std::function<void()> progress_;
    std::chrono::steady_clock::time_point t0_;
    size_t before_msgs_ = 0;
    void log(const std::string& msg);
    void pump();
};

} // namespace agent

#endif // AGENT_COMPRESSOR_H
