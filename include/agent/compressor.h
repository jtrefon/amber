
#ifndef AGENT_COMPRESSOR_H
#define AGENT_COMPRESSOR_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/llm.h"
#include "agent/session_brief.h"

namespace agent {

class Context;
struct AgentHooks;
struct CompressionObserver;
class MemoryStore;
struct Memory;
struct Skill;
struct ExtractionItem;

// Single source of truth for the default compression threshold. The gate,
// load_compression_config, and the UIs all read this value.
inline constexpr double kDefaultCompressionThreshold = 0.70;

// Default post-compression target: 10% of the context window. Compression
// keeps archiving older core turns until the output is ~this fraction.
inline constexpr int kDefaultCompressionTargetPct = 10;

// Default number of the most-recent user prompts carried over verbatim so the
// agent can continue the active task after compression.
inline constexpr int kDefaultCompressionKeepLastPrompts = 10;

// Longest summary the pipeline accepts from the classifier; anything longer
// is truncated so a slop response cannot bloat the archive message.
inline constexpr std::size_t kMaxSummaryChars = 200;

// Placeholder that replaces bulky old tool outputs in Phase-0 pruning. The
// session log retains the original content.
inline constexpr char kToolOutputOmitted[] =
    "[tool output omitted \u2014 session log retains the original]";

// The archive is bookkeeping: a hard cap keeps it from eating the budget the
// pass is trying to reach. Oldest entries merge into one line.
inline constexpr std::size_t kMaxArchiveEntries = 32;
inline constexpr std::size_t kMaxArchiveTokens = 800;

// The archive keeps its newest entries verbatim. Older ones are folded into a
// single entry by one extra LLM call once the archive outgrows its cap; below
// the cap there is nothing to fold and the pipeline spends no call.
inline constexpr std::size_t kArchiveKeepVerbatim = 8;

// The archive entries that should be folded: the oldest ones, or empty when the
// archive is inside its cap (entry count and token budget). Folding is worth a
// request only when what is left can actually be summarised.
json archive_fold_candidates(const json& archive);

// Replace the folded entries with one summary and keep the newest `keep`
// verbatim. Monotonic: an empty summary, or one that does not make the archive
// smaller, returns `archive` unchanged.
json apply_archive_fold(const json& archive, const std::string& folded, std::size_t keep);

// The one extra request: the entries to fold, and what the reply should be.
Message build_archive_fold_request(const json& entries);

// The model's reply as one archive summary: trimmed, code fences stripped,
// empty when it carries nothing usable.
std::string parse_archive_fold_response(const std::string& reply);

// Fold the previous archive in place when it has outgrown its cap: the oldest
// entries become one summary, the newest stay verbatim. One standalone LLM
// request, and only when there is something to fold. Non-fatal by design: a
// failed call or a fold that does not shrink the archive leaves it untouched for
// cap_archive() to merge. Returns true when the archive got smaller.
bool fold_stale_archive(std::vector<Message>& history, LLMClient& client,
                        CompressionObserver* observer);

// Prefix of the compressed-context message produced by apply_classification;
// enforce_headroom locates it by this prefix to append archive entries.
inline constexpr char kCompressedContextPrefix[] = "Compressed conversation context:";

// True when a message is a harness notice ([harness] prefix, Message::harness
// set): the environment reporting a retry, a repair or a budget rather than the
// user speaking. The compression guard counts only genuine user prompts, so a
// notice must not inflate the protected recent tail.
inline bool is_harness_notice(const Message& m) noexcept {
    return m.harness;
}

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

enum class Classification : std::uint8_t { core, context, prune };

struct CompressionResult {
    size_t messages_before = 0;
    size_t messages_after = 0;
    size_t tokens_before = 0;
    size_t tokens_after = 0;
    // Conversation-only tokens (everything except `system` messages), before
    // and after. The system prompt is immutable, so including it in a
    // compaction ratio hides the effect: 8.7K of an 8.8K context is the prompt.
    // `payload_tokens_after` includes the archive the pass wrote.
    size_t payload_tokens_before = 0;
    size_t payload_tokens_after = 0;
    size_t core_count = 0;
    size_t context_count = 0;
    size_t prune_count = 0;
    // Tokens of the compressed-context message the pass wrote (summary +
    // archive). Zero when the pass produced no reduction and kept the history.
    size_t archive_tokens = 0;
    std::string error; // non-empty when compression failed
};

struct CompressionConfig {
    double threshold = kDefaultCompressionThreshold;
    int min_turns = 10;
    int cooldown_turns = 20;
    int context_size = 0; // filled from Config for headroom enforcement
    // Post-compression target (% of context window) and how many of the most
    // recent user prompts survive verbatim. Defaults match the pipeline.
    int target_pct = kDefaultCompressionTargetPct;
    int keep_last_prompts = kDefaultCompressionKeepLastPrompts;
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
    std::string name; // human-readable label
    std::string content;
    std::vector<std::string> tags;
    std::string action;         // "upsert" or "deprecate"
    std::string trigger_phrase; // only for skills
};

// Structured result of parsing the LLM compression response.
struct CompressionResponse {
    std::vector<ClassifiedSegment> segments;
    std::vector<KnowledgeOp> memory_ops;
    std::vector<KnowledgeOp> skill_ops;
    // Coherent work-state summary the classifier produced ("what's being
    // worked on, what's done, what remains"). Placed in the compressed-context
    // message so the agent can continue from the summary + preserved tail.
    std::string summary;
    // Session brief extracted by the extract step (intent/direction/done/
    // next/avoid). Absent when the LLM did not produce one — non-fatal
    // (memories/skills still apply). See docs/plan/session-brief.md.
    std::optional<SessionBrief> brief;
    std::string error; // non-empty when the pipeline failed;
                       // callers must keep the context untouched
};

// The request prefix a host sends on a normal turn: the blocks injected after
// the system prompt, the blocks appended after the conversation, and the tool
// schemas. Replaying it makes the classify/extract calls extensions of the
// cached prefix instead of a cold re-prefill of the whole context. The pipeline
// only reads it; the host owns the blocks and the tool leases.
struct CompressionRequestPrefix {
    std::vector<Message> head_blocks;
    std::vector<Message> tail_blocks;
    std::vector<std::shared_ptr<Tool>> tools;
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
    // Token usage for the classify/extract request just completed.
    virtual void on_llm_usage(const Stats& /*stats*/) {}
    virtual void on_parse_result(const CompressionResponse& /*cr*/) {}
    virtual void on_apply_result(const CompressionResult& /*r*/) {}
    virtual void on_error(const std::string& /*msg*/) {}
    virtual void on_compress_done(const CompressionResult& /*r*/) {}
    // Intermediate progress: fired as the pipeline moves through phases so a
    // host can render a live (decreasing) context gauge. tokens_remaining /
    // msgs_remaining are the current working estimate, which shrinks as
    // collapse/prune/apply drop messages.
    virtual void on_progress(size_t /*tokens_remaining*/, size_t /*msgs_remaining*/) {}
};

class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const Context& context, const Config& agent_cfg) const = 0;
    virtual void set_last_compress_turn(size_t turn) { (void)turn; }
    virtual bool is_within_cooldown(size_t current_turn) const {
        (void)current_turn;
        return false;
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
    // READ — classify/extract requests are assembled from the snapshot plus
    // `prefix`, so a failed pipeline leaves the context untouched by
    // construction (spec invariant 7). The caller rebuilds the context from the
    // returned message list on success.
    // `prefix` is the live request's head/tail blocks and tool schemas. Passing
    // it is what lets both LLM calls extend the server's cached prefix; an empty
    // prefix still works (a cold prefill, as a host with no injected blocks and
    // no tools would see).
    virtual std::vector<Message> compress(const Context& context, const CompressionConfig& cfg,
                                          LLMClient& client,
                                          CompressionObserver* observer = nullptr,
                                          CompressionResponse* response_out = nullptr,
                                          const CompressionRequestPrefix& prefix = {}) = 0;
};

// ---------------------------------------------------------------------------
// Pipeline modules (free functions)
// ---------------------------------------------------------------------------

// Collapse detected loops in history (modifies in place).
void collapse_loops(std::vector<Message>& history);

// Phase-0 cheap pass: replace bulky tool outputs (>200 chars) with a short
// placeholder. When `cfg` is non-null the whole history is pruned EXCEPT the
// protected tail (the last keep_last_prompts real user turns and everything
// after them), and then the protected region is trimmed by
// prune_in_turn_results. With cfg null only messages older than the legacy
// two-user guard are pruned. Returns the number of messages replaced.
std::size_t prune_tool_io(std::vector<Message>& history, const CompressionConfig* cfg = nullptr);

// Replace one tool message with a recallable digest, storing the payload on
// demand when the envelope has no handle. Returns false for a message that is
// already a digest (the pass stays idempotent).
bool digest_tool_message(Message& message);

// Keep the newest tool results inside the protected tail that fit its token
// budget and digest the older ones. This is what gives a single oversized turn
// relief: the final user prompt and every assistant message always survive, the
// newest evidence survives, and each replacement carries a `result_id` so
// read_result can restore it. Deterministic: no LLM call, no summary. Returns
// the number of messages replaced (0 for an unknown window or a turn already
// inside the budget).
std::size_t prune_in_turn_results(std::vector<Message>& history,
                                  const CompressionConfig* cfg = nullptr);

// Tool messages of the protected tail that `prune_in_turn_results` would
// replace, newest-first walk and budget applied. Pure: no store, no mutation.
// The replay harness measures with it; the pass below applies it.
std::vector<std::size_t> in_turn_digest_candidates(const std::vector<Message>& history,
                                                   const CompressionConfig& cfg);

// True when the protected tail is the whole conversation: the final prompt and
// everything after it, with no older turn for the classifier to archive. That
// is the single-oversized-turn case, whose only relief is in-turn pruning.
bool tail_is_whole_conversation(const std::vector<Message>& history, const CompressionConfig& cfg);

// Repair tool_call/tool_result group splits left by classification: orphaned
// tool messages (no preceding assistant tool_calls) are removed, and
// assistant tool_calls whose results were pruned get a stub tool message
// injected so the message sequence stays API-valid.
void sanitize_tool_pairs(std::vector<Message>& history);

// Build a classification-only request (returns array of turn tags).
// This is the first step of the multi-step pipeline. `history` supplies the
// index legend the classifier needs; the legend is appended to the instruction,
// so the conversation prefix stays byte-identical (KV reuse). When
// `update_previous` is true the conversation already carries a compressed-context
// block: the classifier is told to extend its archive instead of re-summarising.
Message build_classify_request(bool update_previous, const std::vector<Message>& history);

// Build an extraction request (returns memories + skills).
// This is the second step, appended after the classification response.
Message build_extract_request();

// Parse the LLM's JSON response into a CompressionResponse.
CompressionResponse parse_compression_response(const std::string& json);

// Apply classification segments to history, returning compressed history.
// When `cfg` is null the legacy defaults apply (protect the last two user
// turns, no target-budget floor); the pipeline passes the real config so the
// configurable keep-last-prompts guard and target_pct budget are honored.
std::vector<Message> apply_classification(const std::vector<Message>& history,
                                          const CompressionResponse& response,
                                          const CompressionConfig* cfg = nullptr);

// Apply memory/skill upsert/deprecate ops to a MemoryStore.
// When `items` is non-null, it receives one ExtractionItem per op for UI
// reporting. The store owns promotion thresholds.
void apply_memory_ops(MemoryStore& store, const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path, std::vector<ExtractionItem>* items = nullptr);
void apply_skill_ops(MemoryStore& store, const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path, std::vector<ExtractionItem>* items = nullptr);

// Enforce that the compressed context leaves at least 25% headroom.
// Drops oldest non-system messages, recording them as archive entries on
// the compressed-context message. The system prompt at index 0 is never
// modified, and neither is the protected tail (the newest real user turns that
// fit the post-compaction target, always including the final prompt). Returns
// the tightened vector (it may be shorter than input).
std::vector<Message> enforce_headroom(std::vector<Message> compressed, size_t context_size,
                                      const CompressionConfig& cfg);

// Enforce the post-compression target budget (context_size * target_pct / 100)
// by archiving eligible oldest core messages into the compressed-context
// archive until the output fits. The last `keep_last_prompts` user messages
// and the system prompt are never archived. Returns the trimmed vector.
std::vector<Message> enforce_target_budget(std::vector<Message> compressed, size_t context_size,
                                           const CompressionConfig& cfg);

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

// The pipeline is stateless: it reads the configuration it is handed on each
// compress() call, because settings can change while a session runs.
std::unique_ptr<CompressionStrategy> make_compressor();

std::unique_ptr<CompressionGate> make_compression_gate(const CompressionConfig& cfg);

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
    void on_llm_usage(const Stats& stats) override;
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
