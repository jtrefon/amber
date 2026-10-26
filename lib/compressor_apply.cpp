
#include "agent/compressor.h"
#include "agent/agent_helpers.h"
#include "agent/context.h"
#include "agent/experience.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_set>

namespace agent {

namespace {

// A message that begins with the compressed-context marker is a previous
// compression pass, not conversation.
bool is_compressed_context(const std::string& content) {
    return content.compare(0, sizeof(kCompressedContextPrefix) - 1, kCompressedContextPrefix) == 0;
}

// Per-turn tags and summaries from the classifier's segments. Indices are
// clamped to the history: the classifier counts turns, and may overrun.
struct TaggedTurns {
    std::vector<Classification> tags;
    std::vector<std::string> summaries;
};

TaggedTurns tag_turns(const std::vector<Message>& history, const CompressionResponse& response) {
    TaggedTurns out;
    out.tags.assign(history.size(), Classification::core);
    out.summaries.resize(history.size());
    for (const auto& seg : response.segments) {
        const size_t start = std::min(seg.turn_start, history.size() - 1);
        const size_t end = std::min(seg.turn_end, history.size() - 1);
        for (size_t i = start; i <= end; ++i) {
            out.tags[i] = seg.tag;
            out.summaries[i] = seg.summary.size() > kMaxSummaryChars
                                   ? seg.summary.substr(0, kMaxSummaryChars)
                                   : seg.summary;
        }
    }
    return out;
}

// Safety net: the protected tail (from `tail_start` to the end) survives
// verbatim — a misclassification must never drop the active task. The boundary
// is computed once by protected_tail_start.
void protect_active_task(std::vector<Classification>& tags, size_t tail_start) {
    for (size_t i = tail_start; i < tags.size(); ++i)
        tags[i] = Classification::core;
}

// Archive entries carried over from a previous compressed-context message:
// re-compression updates the archive instead of replacing it. The last such
// message wins.
json carried_archive(const std::vector<Message>& history, size_t start_idx) {
    json previous = json::array();
    for (size_t i = start_idx; i < history.size(); ++i) {
        if (!is_compressed_context(history[i].content))
            continue;
        auto prev = json::parse(history[i].content.substr(sizeof(kCompressedContextPrefix) - 1),
                                nullptr, false);
        if (!prev.is_discarded() && prev.is_object() && prev.contains("archive") &&
            prev["archive"].is_array())
            previous = prev["archive"];
    }
    return previous;
}

// One run of contiguous context-tagged messages, summarised in the archive.
struct ArchiveSeg {
    size_t start = 0;
    size_t end = 0;
    std::string summary;
};

// Split the history into what survives verbatim (core) and what is summarised
// into archive segments. The saved system prompt is skipped (it is prepended
// again later) and a previous compressed-context message is consumed, never
// duplicated in the output.
struct Partition {
    std::vector<Message> core;
    std::vector<ArchiveSeg> archive;
};

Partition partition_messages(const std::vector<Message>& history,
                             const std::vector<Classification>& tags,
                             const std::vector<std::string>& summaries, size_t start_idx) {
    Partition out;
    for (size_t i = start_idx; i < history.size(); ++i) {
        if (is_compressed_context(history[i].content))
            continue;
        switch (tags[i]) {
        case Classification::core:
            out.core.push_back(history[i]);
            break;
        case Classification::context:
            if (out.archive.empty() || out.archive.back().end != i - 1)
                out.archive.push_back({i, i, summaries[i]}); // non-contiguous: new segment
            else
                out.archive.back().end = i; // contiguous: extend
            break;
        case Classification::prune:
            break;
        }
    }
    return out;
}

// Merge the oldest entries until the archive fits its cap. The exact spans stop
// mattering once they are compressed; one line says they are still there.
void cap_archive(json& archive) {
    if (!archive.is_array())
        return;
    while (archive.size() > 1 &&
           (archive.size() > kMaxArchiveEntries || archive.dump().size() / 4 > kMaxArchiveTokens)) {
        json out = json::array();
        out.push_back({{"turns", "earlier"}, {"summary", "earlier work, compressed"}});
        for (std::size_t i = 2; i < archive.size(); ++i)
            out.push_back(archive[i]);
        archive = std::move(out);
    }
}

// The archive carried on the compressed-context message: previous entries first
// (re-compression carries them forward), then this pass's segments. Bounded:
// the oldest entries merge into one line instead of riding along forever.
json archive_json_for(const json& previous, const std::vector<ArchiveSeg>& segments) {
    json out = previous;
    for (const auto& seg : segments) {
        const std::string range = (seg.start == seg.end)
                                      ? std::to_string(seg.start)
                                      : std::to_string(seg.start) + "-" + std::to_string(seg.end);
        out.push_back(
            {{"turns", range}, {"summary", seg.summary.empty() ? "(compressed)" : seg.summary}});
    }
    cap_archive(out);
    return out;
}

// The classifier's work-state summary is the anchor the agent continues from
// after compression. The archive is the only other payload: everything the pass
// used to write here (`type`, `version`, `facts.last_goal`) had no reader.
Message compressed_context_message(const CompressionResponse& response, const json& archive) {
    json ctx;
    ctx["archive"] = archive;
    if (!response.summary.empty())
        ctx["summary"] = response.summary;

    Message out;
    out.role = "system";
    out.content = std::string(kCompressedContextPrefix) + "\n" + ctx.dump(2);
    return out;
}

// Minimum context invariant: at least one real user message must survive, or the
// agent has no task to continue from. Harness notices ride the user role but are
// the environment speaking, so they do not count as a task.
void ensure_user_message(std::vector<Message>& core, const std::vector<Message>& history) {
    const auto is_task = [](const Message& m) { return m.role == "user" && !is_harness_notice(m); };
    if (std::any_of(core.begin(), core.end(), is_task))
        return;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (is_task(*it)) {
            core.push_back(*it);
            return;
        }
    }
}

// Token budget a compression pass targets: `pct` of the context window.
size_t target_tokens(size_t context_size, int pct) {
    const int p = pct > 0 ? pct : kDefaultCompressionTargetPct;
    return static_cast<size_t>(static_cast<double>(context_size) * static_cast<double>(p) / 100.0);
}

// Index of the compressed-context archive message, or size() when absent.
size_t find_compressed_context(const std::vector<Message>& messages) {
    for (size_t i = 0; i < messages.size(); ++i)
        if (is_compressed_context(messages[i].content))
            return i;
    return messages.size();
}

// Tokens in [from, to).
size_t tokens_in_range(const std::vector<Message>& messages, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from; i < to && i < messages.size(); ++i)
        n += message_tokens(messages[i]);
    return n;
}

// Token budget the protected tail may occupy: the post-compression target minus
// a reserve for the compressed-context message the pass writes. With no known
// window there is no token bound — the prompt cap rules.
size_t tail_budget_tokens(size_t context_size, const CompressionConfig& cfg) {
    if (context_size == 0)
        return std::numeric_limits<size_t>::max();
    const size_t target = target_tokens(context_size, cfg.target_pct);
    if (target == 0)
        return std::numeric_limits<size_t>::max();
    const size_t reserve = std::max<size_t>(512, target / 10);
    return target > reserve ? target - reserve : target;
}

// Boundary of the protected tail: the newest real user turns (at most
// `keep_last_prompts`) that fit the tail budget, always including the final
// prompt and everything after it. The final prompt survives even when it alone
// exceeds the budget — a safety net that drops the task is not a safety net.
size_t protected_tail_start(const std::vector<Message>& messages, const CompressionConfig& cfg,
                            size_t context_size) {
    std::vector<size_t> users;
    for (size_t i = 1; i < messages.size(); ++i)
        if (messages[i].role == "user" && !is_harness_notice(messages[i]))
            users.push_back(i);
    if (users.empty())
        return messages.size();
    const auto keep_max = static_cast<size_t>(
        cfg.keep_last_prompts > 0 ? cfg.keep_last_prompts : kDefaultCompressionKeepLastPrompts);
    const size_t budget = tail_budget_tokens(context_size, cfg);
    size_t start = users.back();
    size_t used = tokens_in_range(messages, start, messages.size());
    size_t kept = 1;
    for (size_t idx = users.size() - 1; idx > 0; --idx) {
        if (kept >= keep_max)
            break;
        const size_t candidate = users[idx - 1];
        const size_t turn = tokens_in_range(messages, candidate, start);
        if (used + turn > budget)
            break;
        used += turn;
        start = candidate;
        ++kept;
    }
    return start;
}

// Legacy boundary for callers with no config: the last `keep` prompts.
size_t protected_tail_start(const std::vector<Message>& messages, int keep) {
    std::vector<size_t> users;
    for (size_t i = 1; i < messages.size(); ++i)
        if (messages[i].role == "user" && !is_harness_notice(messages[i]))
            users.push_back(i);
    auto protect = static_cast<size_t>(keep);
    if (protect > users.size())
        protect = users.size();
    return protect > 0 ? users[users.size() - protect] : messages.size();
}

// Append the dropped messages to the archive block as one compact entry, so the
// carried summary still reflects what was removed without spending a token per
// dropped message. `dropped` holds the pre-erase indices, so the entry names the
// span that went.
void append_archived_turns(std::vector<Message>& messages, size_t ctx_idx,
                           const std::vector<size_t>& dropped, const char* note) {
    if (dropped.empty())
        return;
    const std::string json_part =
        messages[ctx_idx].content.substr(sizeof(kCompressedContextPrefix) - 1);
    json body = json::parse(json_part, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return;
    json archive = body.value("archive", json::array());
    const std::string range =
        dropped.front() == dropped.back()
            ? std::to_string(dropped.front())
            : std::to_string(dropped.front()) + "-" + std::to_string(dropped.back());
    archive.push_back({{"turns", range}, {"summary", note}});
    cap_archive(archive);
    body["archive"] = std::move(archive);
    messages[ctx_idx].content = std::string(kCompressedContextPrefix) + "\n" + body.dump(2);
}

// Estimated tokens one archive entry adds. A drop must free at least this much
// for the budget to converge, so the entry cost is reserved before measuring.
constexpr size_t kArchiveEntryTokenReserve = 32;

// Headroom leaves the history below the trigger plus this margin, never above
// 75% even when the trigger is configured that high.
constexpr double kHeadroomMargin = 0.10;
constexpr double kHeadroomCeiling = 0.75;

// A pass may need more than one round: the repair injects stubs, which the next
// measurement accounts for. Bounded so an unbounded tail cannot livelock it.
constexpr int kMaxBudgetPasses = 8;

// Drop the oldest non-system messages before `tail_start` until the history fits
// `budget` minus the entry that records the drop. Returns true when it removed
// anything. `dropped` receives the pre-erase indices.
bool drop_oldest_to_fit(std::vector<Message>& messages, size_t budget, size_t tail_start,
                        std::vector<size_t>& dropped) {
    if (budget <= kArchiveEntryTokenReserve)
        return false;
    const size_t target = budget - kArchiveEntryTokenReserve;
    size_t used = estimate_tokens(messages);
    for (size_t i = 1; i < tail_start && i < messages.size();) {
        if (messages[i].role == "system") {
            ++i;
            continue;
        }
        if (used <= target)
            break;
        used -= message_tokens(messages[i]);
        dropped.push_back(i);
        messages.erase(messages.begin() + static_cast<ptrdiff_t>(i));
        // The body only runs while 1 < tail_start, so this cannot underflow.
        --tail_start;
    }
    return !dropped.empty();
}

// Drop oldest non-system messages until the estimated tokens fit `budget`, then
// record each run in the compressed-context archive under `note`. Messages at or
// after the protected tail start are never touched, so the active task survives
// even when the tail alone exceeds the budget. After every pass the history is
// repaired (tool pairing) and re-checked, so the budget accounts for the archive
// the pass just wrote.
void archive_until_under_budget(std::vector<Message>& compressed, size_t budget,
                                const CompressionConfig& cfg, size_t context_size,
                                const char* note) {
    if (estimate_tokens(compressed) <= budget)
        return;
    const std::vector<Message> before = compressed;
    for (int pass = 0; pass < kMaxBudgetPasses; ++pass) {
        if (estimate_tokens(compressed) <= budget)
            break;
        std::vector<size_t> dropped;
        if (!drop_oldest_to_fit(compressed, budget,
                                protected_tail_start(compressed, cfg, context_size), dropped))
            break;
        const size_t ctx_idx = find_compressed_context(compressed);
        if (ctx_idx != compressed.size())
            append_archived_turns(compressed, ctx_idx, dropped, note);
        sanitize_tool_pairs(compressed);
        ensure_user_message(compressed, before);
    }
    sanitize_tool_pairs(compressed);
    ensure_user_message(compressed, before);
}

// An assistant message carrying a usable tool_calls array.
bool is_assistant_tool_call(const Message& m) {
    return m.role == "assistant" && !m.tool_calls.is_null() && m.tool_calls.is_array() &&
           !m.tool_calls.empty();
}

// A tool result is only valid directly after an assistant message carrying
// tool_calls; anything else is an orphan and must be dropped.
bool is_orphan_tool_result(const std::vector<Message>& out) {
    return out.empty() || !is_assistant_tool_call(out.back());
}

// The stand-in result for an assistant tool_calls message whose real result was
// pruned, so the rebuilt history stays API-valid.
Message tool_stub_for(const Message& m) {
    Message stub;
    stub.role = "tool";
    stub.content = kToolOutputOmitted;
    if (m.tool_calls[0].is_object() && m.tool_calls[0].contains("id") &&
        m.tool_calls[0]["id"].is_string())
        stub.tool_call_id = m.tool_calls[0]["id"].get<std::string>();
    return stub;
}

} // namespace

// Rebuild a conversation from the classifier's per-turn tags: core turns
// survive verbatim, context turns collapse into an archive, prune turns are
// dropped. The active task, the system prompt and at least one user message are
// protected regardless of how the classifier tagged them.
std::vector<Message> apply_classification(const std::vector<Message>& history,
                                          const CompressionResponse& response,
                                          const CompressionConfig* cfg) {
    if (history.empty() || response.segments.empty())
        return history;

    // How many of the most-recent user prompts survive verbatim, and how much
    // tail the token budget allows. Direct callers without cfg keep the legacy
    // two-prompt guard.
    const size_t tail_start =
        cfg ? protected_tail_start(history, *cfg, static_cast<size_t>(cfg->context_size))
            : protected_tail_start(history, /*keep=*/2);

    TaggedTurns turns = tag_turns(history, response);
    protect_active_task(turns.tags, tail_start);

    // Always preserve the system prompt (index 0) regardless of how the
    // classifier tagged it — the classifier treats message indices as "turn"
    // numbers and may accidentally prune or archive the system message.
    Message saved_system;
    if (history[0].role == "system")
        saved_system = history[0];
    const size_t start_idx = saved_system.role == "system" ? 1 : 0;

    Partition part = partition_messages(history, turns.tags, turns.summaries, start_idx);
    // The facts scan sees the core turns only, so the message is built before
    // it is appended.
    Message compressed = compressed_context_message(
        response, archive_json_for(carried_archive(history, start_idx), part.archive));
    part.core.push_back(std::move(compressed));
    ensure_user_message(part.core, history);

    // Prepend the saved system prompt so the classifier cannot remove it.
    if (saved_system.role == "system")
        part.core.insert(part.core.begin(), std::move(saved_system));

    // Repair tool_call/tool_result group splits introduced by pruning or
    // misclassification so the rebuilt history stays API-valid.
    sanitize_tool_pairs(part.core);
    return part.core;
}

// ---------------------------------------------------------------------------
// Target-budget enforcement
// ---------------------------------------------------------------------------

// After apply_classification the output may still be far above the desired
// post-compression occupancy (the classifier + protected tail can keep 50%+ of
// a large window). This pass walks the output oldest-first and ARCHIVES
// eligible core messages — moving their content into the compressed-context
// archive block with a "(compressed)" summary — until the estimated tokens fit
// `context_size * target_pct / 100`. Protected from archiving: the system
// prompt (index 0), the compressed-context message itself, and the trailing
// span covered by the last `keep_last_prompts` user messages (the active task
// carried over verbatim). Returns the trimmed output (may be unchanged).
std::vector<Message> enforce_target_budget(std::vector<Message> compressed, size_t context_size,
                                           const CompressionConfig& cfg) {
    if (context_size == 0)
        return compressed;
    const size_t target = target_tokens(context_size, cfg.target_pct);
    if (target == 0)
        return compressed;

    // The compressed-context archive message carries the JSON block we extend.
    // It must exist (apply_classification always emits one); if not, bail — do
    // not fabricate.
    if (find_compressed_context(compressed) == compressed.size())
        return compressed;

    // The archive writes backwards from the protected tail; it computes the
    // boundary itself so the target and headroom passes share one rule.
    archive_until_under_budget(compressed, target, cfg, context_size,
                               "(compressed to meet target budget)");
    return compressed;
}

std::size_t prune_tool_io(std::vector<Message>& history, const CompressionConfig* cfg) {
    // Bulky tool outputs are the largest single token class in agent sessions
    // (multi-KB bash/read results). When the pipeline runs (cfg non-null) the
    // WHOLE history is eligible EXCEPT the protected tail: the gate fires at the
    // top of a request, so the newest results belong to the turn the model is
    // about to reason about and must survive verbatim. With cfg null (direct
    // legacy callers) only messages older than the two-user guard are pruned.
    size_t limit;
    if (cfg) {
        limit = protected_tail_start(history, *cfg, static_cast<size_t>(cfg->context_size));
    } else {
        // Legacy: prune only messages before the second-to-last user turn.
        size_t last_user = history.size();
        size_t prev_user = history.size();
        for (size_t i = 1; i < history.size(); ++i) {
            if (history[i].role == "user") {
                prev_user = last_user;
                last_user = i;
            }
        }
        limit = prev_user < history.size() ? prev_user : last_user;
    }
    std::size_t replaced = 0;
    for (size_t i = 1; i < limit; ++i) {
        Message& m = history[i];
        if (m.role == "tool" && m.content.size() > 200) {
            m.content = tool_result_digest(m);
            ++replaced;
        }
    }
    return replaced;
}

void sanitize_tool_pairs(std::vector<Message>& history) {
    std::vector<Message> out;
    out.reserve(history.size());
    for (size_t i = 0; i < history.size(); ++i) {
        const Message& m = history[i];
        if (m.role == "tool") {
            if (!is_orphan_tool_result(out))
                out.push_back(m);
            continue;
        }
        out.push_back(m);
        // An assistant tool_calls message whose result was pruned gets a stub
        // result before the next non-tool message.
        if (is_assistant_tool_call(m) && (i + 1 >= history.size() || history[i + 1].role != "tool"))
            out.push_back(tool_stub_for(m));
    }
    history.swap(out);
}

void apply_memory_ops(MemoryStore& store, const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path, std::vector<ExtractionItem>* items) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            int evidence = store.deprecate(op.content);
            if (evidence >= 0 && items) {
                items->push_back({op.name.empty() ? op.content.substr(0, 40) : op.name, "deprecate",
                                  evidence, evidence > 0});
            }
            continue;
        }
        // Validate name uniqueness: if a memory with the same name but
        // different content exists, flag as conflict so the LLM picks
        // a different name rather than silently overwriting.
        std::string label = op.name.empty() ? op.content.substr(0, 40) : op.name;
        auto existing = store.find_memory(label);
        if (existing && existing->content != op.content) {
            if (items)
                items->push_back(
                    {label, "conflict: name \"" + label + "\" already used for different content",
                     existing->evidence_count, existing->promoted});
            continue;
        }
        Memory mem;
        mem.name = label;
        mem.content = op.content;
        mem.tags = op.tags;
        mem.evidence_count = store.memory_promote_threshold();
        mem.promoted = true;
        store.upsert(mem);
        if (items)
            items->push_back({label, "upsert", mem.evidence_count, true});
    }

    if (!store_path.empty())
        store.save(store_path);
}

void apply_skill_ops(MemoryStore& store, const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path, std::vector<ExtractionItem>* items) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            int evidence = store.deprecate(op.content);
            if (evidence >= 0 && items) {
                items->push_back({op.name.empty() ? op.content.substr(0, 40) : op.name, "deprecate",
                                  evidence, evidence > 0});
            }
            continue;
        }
        std::string label = op.name.empty() ? op.content.substr(0, 40) : op.name;
        auto existing = store.find_skill(label);
        if (existing && existing->content != op.content) {
            if (items)
                items->push_back(
                    {label, "conflict: name \"" + label + "\" already used for different content",
                     existing->evidence_count, existing->promoted});
            continue;
        }
        Skill sk;
        sk.name = label;
        sk.content = op.content;
        sk.tags = op.tags;
        sk.trigger_phrase = op.trigger_phrase;
        sk.evidence_count = store.skill_promote_threshold();
        sk.promoted = true;
        store.upsert(sk);
        if (items)
            items->push_back({label, "upsert", sk.evidence_count, true});
    }

    if (!store_path.empty())
        store.save(store_path);
}

// ---------------------------------------------------------------------------
// Budget enforcement
// ---------------------------------------------------------------------------

// Enforce that the compressed context leaves at least 25% headroom.
// Oldest non-system messages are dropped oldest-first and recorded as archive
// entries on the compressed-context message — never replaced by placeholder
// messages (the LLM must not see fabricated system turns). The protected tail
// (the last `keep_last_prompts` real user turns and everything after them) is
// never touched: a safety net that deletes the task is not a safety net.
std::vector<Message> enforce_headroom(std::vector<Message> compressed, size_t context_size,
                                      const CompressionConfig& cfg) {
    if (context_size == 0)
        return compressed;
    // The safety net must leave the history BELOW the trigger that fired it, or
    // a compaction can finish in a state that qualifies again as soon as the
    // cooldown lapses. Derive it from the threshold (with a margin) instead of
    // a fixed 75% that sits above the default 70%.
    const double fraction =
        std::min(kHeadroomCeiling, std::max(0.10, cfg.threshold - kHeadroomMargin));
    const auto budget = static_cast<size_t>(static_cast<double>(context_size) * fraction);
    archive_until_under_budget(compressed, budget, cfg, context_size, "(over-budget archived)");
    return compressed;
}

} // namespace agent
