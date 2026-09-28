
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"

#include <algorithm>
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

// Safety net: the last `keep_prompts` REAL user prompts and everything after
// them survive verbatim — a misclassification must never drop the active task.
// Internal confirmation probes ("Are you finished?") are not real prompts and
// must not count toward (or inflate) the protected tail.
void protect_active_task(const std::vector<Message>& history, std::vector<Classification>& tags,
                         int keep_prompts) {
    std::vector<size_t> user_positions;
    for (size_t i = 1; i < history.size(); ++i)
        if (history[i].role == "user" && !is_confirmation_probe(history[i]))
            user_positions.push_back(i);
    if (user_positions.empty())
        return;
    auto protect = static_cast<size_t>(keep_prompts);
    if (protect > user_positions.size())
        protect = user_positions.size();
    for (size_t i = user_positions[user_positions.size() - protect]; i < tags.size(); ++i)
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

// The archive carried on the compressed-context message: previous entries first
// (re-compression carries them forward), then this pass's segments.
json archive_json_for(const json& previous, const std::vector<ArchiveSeg>& segments) {
    json out = previous;
    for (const auto& seg : segments) {
        const std::string range = (seg.start == seg.end)
                                      ? std::to_string(seg.start)
                                      : std::to_string(seg.start) + "-" + std::to_string(seg.end);
        out.push_back(
            {{"turns", range}, {"summary", seg.summary.empty() ? "(compressed)" : seg.summary}});
    }
    return out;
}

// The classifier's work-state summary is the anchor the agent continues from
// after compression; carry it, plus the last short user goal, on the message.
Message compressed_context_message(const CompressionResponse& response, const json& archive,
                                   const std::vector<Message>& core) {
    json ctx;
    ctx["type"] = "compressed_context";
    ctx["version"] = 1;
    ctx["archive"] = archive;
    if (!response.summary.empty())
        ctx["summary"] = response.summary;

    json facts = json::object();
    for (const auto& msg : core) {
        if (msg.role == "user" && msg.content.size() < 200) {
            facts["last_goal"] = msg.content;
            break;
        }
    }
    ctx["facts"] = facts;

    Message out;
    out.role = "system";
    out.content = std::string(kCompressedContextPrefix) + "\n" + ctx.dump(2);
    return out;
}

// Minimum context invariant: at least one user message must survive, or the
// agent has no task to continue from.
void ensure_user_message(std::vector<Message>& core, const std::vector<Message>& history) {
    if (std::any_of(core.begin(), core.end(), [](const Message& m) { return m.role == "user"; }))
        return;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->role == "user") {
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

// Boundary of the protected tail: the last `keep` real user messages (and
// everything after them) stay verbatim — the same rule as the classify guard.
// The compressed-context message is a trailing system message; ignore it.
size_t protected_tail_start(const std::vector<Message>& messages, int keep) {
    std::vector<size_t> users;
    for (size_t i = 1; i < messages.size(); ++i)
        if (messages[i].role == "user" && !is_confirmation_probe(messages[i]))
            users.push_back(i);
    auto protect = static_cast<size_t>(keep);
    if (protect > users.size())
        protect = users.size();
    return protect > 0 ? users[users.size() - protect] : messages.size();
}

// Append the dropped messages to the archive block as compact entries, so the
// carried summary still reflects what was removed.
void append_archived_turns(std::vector<Message>& messages, size_t ctx_idx,
                           const std::vector<size_t>& archived, const char* note) {
    const std::string json_part =
        messages[ctx_idx].content.substr(sizeof(kCompressedContextPrefix) - 1);
    json body = json::parse(json_part, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        return;
    json archive = body.value("archive", json::array());
    for (const size_t idx : archived)
        archive.push_back({{"turns", std::to_string(idx)}, {"summary", note}});
    body["archive"] = std::move(archive);
    messages[ctx_idx].content = std::string(kCompressedContextPrefix) + "\n" + body.dump(2);
}

// Drop oldest non-system messages until the estimated tokens fit `budget`, then
// record each dropped message in the compressed-context archive under `note`.
// Messages at or after `tail_start` are never touched. The archive message is a
// system role, so the walk never reaches it.
void archive_until_under_budget(std::vector<Message>& compressed, size_t budget, size_t tail_start,
                                const char* note) {
    size_t used = estimate_tokens(compressed);
    if (used <= budget)
        return;
    std::vector<size_t> dropped;
    for (size_t i = 1; i < tail_start && i < compressed.size(); ++i) {
        if (compressed[i].role == "system")
            continue;
        used -= message_tokens(compressed[i]);
        dropped.push_back(i);
        if (used <= budget)
            break;
    }
    if (dropped.empty())
        return;

    // Remove dropped messages (descending so indices stay valid).
    for (auto it = dropped.rbegin(); it != dropped.rend(); ++it)
        compressed.erase(compressed.begin() + static_cast<ptrdiff_t>(*it));

    // Re-locate the archive message after the erase (indices shifted) and
    // record what went. A missing archive block still leaves the drops in place.
    const size_t ctx_idx = find_compressed_context(compressed);
    if (ctx_idx == compressed.size())
        return;
    append_archived_turns(compressed, ctx_idx, dropped, note);
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

    // How many of the most-recent user prompts survive verbatim (the active
    // task guard). The pipeline passes cfg (default 10); direct callers
    // without cfg keep the legacy 2.
    const int keep_prompts = cfg && cfg->keep_last_prompts > 0 ? cfg->keep_last_prompts : 2;

    TaggedTurns turns = tag_turns(history, response);
    protect_active_task(history, turns.tags, keep_prompts);

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
        response, archive_json_for(carried_archive(history, start_idx), part.archive), part.core);
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

    const int keep =
        cfg.keep_last_prompts > 0 ? cfg.keep_last_prompts : kDefaultCompressionKeepLastPrompts;
    archive_until_under_budget(compressed, target, protected_tail_start(compressed, keep),
                               "(compressed to meet target budget)");
    return compressed;
}

std::size_t prune_tool_io(std::vector<Message>& history, const CompressionConfig* cfg) {
    // Bulky tool outputs are the largest single token class in agent sessions
    // (multi-KB bash/read results). When the pipeline runs (cfg non-null) we
    // prune them across the WHOLE history INCLUDING the recent tail — the
    // session log retains the original, and sanitize_tool_pairs keeps the
    // message sequence API-valid. With cfg null (direct legacy callers) only
    // messages older than the two-user guard are pruned.
    size_t limit = history.size(); // cfg path: prune everything
    if (!cfg) {
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
            m.content = kToolOutputOmitted;
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
// Oldest non-system messages are dropped oldest-first and recorded as
// archive entries on the compressed-context message — never replaced by
// placeholder messages (the LLM must not see fabricated system turns).
std::vector<Message> enforce_headroom(std::vector<Message> compressed, size_t context_size) {
    if (context_size == 0)
        return compressed;
    // Last-resort guard: a fixed 75% of the window. The compressed-context
    // message is a system role, so the walk never reaches it.
    const auto budget = static_cast<size_t>(static_cast<double>(context_size) * 0.75);
    archive_until_under_budget(compressed, budget, compressed.size(), "(over-budget archived)");
    return compressed;
}

} // namespace agent
