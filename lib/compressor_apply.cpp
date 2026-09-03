
#include "agent/compressor.h"
#include "agent/context.h"
#include "agent/experience.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace agent {

std::vector<Message> apply_classification(
    const std::vector<Message>& history,
    const CompressionResponse& response,
    const CompressionConfig* cfg) {
    if (history.empty()) return history;
    if (response.segments.empty()) return history;

    // How many of the most-recent user prompts survive verbatim (the active
    // task guard). The pipeline passes cfg (default 10); direct callers
    // without cfg keep the legacy 2.
    const int keep_prompts =
        cfg && cfg->keep_last_prompts > 0 ? cfg->keep_last_prompts : 2;

    // Build per-turn tag array from segments
    std::vector<Classification> tags(history.size(), Classification::core);
    std::vector<std::string> summaries(history.size());

    for (const auto& seg : response.segments) {
        size_t start = std::min(seg.turn_start, history.size() - 1);
        size_t end = std::min(seg.turn_end, history.size() - 1);
        for (size_t i = start; i <= end; ++i) {
            tags[i] = seg.tag;
            summaries[i] = seg.summary.size() > kMaxSummaryChars
                               ? seg.summary.substr(0, kMaxSummaryChars)
                               : seg.summary;
        }
    }

    // Safety net: the last `keep_prompts` user turns and everything after
    // them survive verbatim — a misclassification must never drop the active
    // task. (Legacy default 2; pipeline default 10, configurable.)
    std::vector<size_t> user_positions;
    for (size_t i = 1; i < history.size(); ++i)
        if (history[i].role == "user") user_positions.push_back(i);
    size_t guard_start = history.size();
    if (!user_positions.empty()) {
        auto protect = static_cast<size_t>(keep_prompts);
        if (protect > user_positions.size()) protect = user_positions.size();
        guard_start = user_positions[user_positions.size() - protect];
    }
    for (size_t i = guard_start; i < history.size(); ++i)
        tags[i] = Classification::core;

    // Always preserve the system prompt (index 0) regardless of how the
    // classifier tagged it — the classifier treats message indices as "turn"
    // numbers and may accidentally prune or archive the system message.
    Message saved_system;
    if (!history.empty() && history[0].role == "system")
        saved_system = history[0];

    // Separate messages by tag, skipping the saved system prompt (it will
    // be prepended before returning so the classifier cannot prune/archive it).
    std::vector<Message> core;
    struct ArchiveSeg { size_t start; size_t end; std::string summary; };
    std::vector<ArchiveSeg> archive_segments;
    size_t prune_count = 0;

    // Archive entries from a previous compressed-context message carry over:
    // re-compression updates the archive instead of replacing it. The old
    // compressed message itself is consumed (never duplicated in the output).
    json previous_archive = json::array();
    size_t start_idx = saved_system.role == "system" ? 1 : 0;
    for (size_t i = start_idx; i < history.size(); ++i) {
        if (history[i].content.compare(
                0, sizeof(kCompressedContextPrefix) - 1,
                kCompressedContextPrefix) == 0) {
            auto prev = json::parse(
                history[i].content.substr(sizeof(kCompressedContextPrefix) - 1),
                nullptr, false);
            if (!prev.is_discarded() && prev.is_object() &&
                prev.contains("archive") && prev["archive"].is_array())
                previous_archive = prev["archive"];
            continue;
        }
        switch (tags[i]) {
            case Classification::core:
                core.push_back(history[i]);
                break;
            case Classification::context:
                if (archive_segments.empty() ||
                    archive_segments.back().end != i - 1) {
                    // Non-contiguous: start a new segment
                    archive_segments.push_back({i, i, summaries[i]});
                } else {
                    // Contiguous: extend the current segment
                    archive_segments.back().end = i;
                }
                break;
            case Classification::prune:
                ++prune_count;
                break;
        }
    }

    // Build archive JSON: previous entries first (re-compression carries
    // them forward), then the new segments from this pass.
    json archive_json = previous_archive;
    for (const auto& seg : archive_segments) {
        json entry;
        std::string range = (seg.start == seg.end)
            ? std::to_string(seg.start)
            : std::to_string(seg.start) + "-" + std::to_string(seg.end);
        entry["turns"] = range;
        entry["summary"] = seg.summary.empty() ? "(compressed)" : seg.summary;
        archive_json.push_back(entry);
    }

    // Build compressed context message
    json ctx;
    ctx["type"] = "compressed_context";
    ctx["version"] = 1;
    ctx["archive"] = archive_json;
    // The classifier's work-state summary is the anchor the agent continues
    // from after compression; carry it on the compressed-context message.
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

    Message compressed_msg;
    compressed_msg.role = "system";
    compressed_msg.content =
        std::string(kCompressedContextPrefix) + "\n" + ctx.dump(2);
    core.push_back(compressed_msg);

    // Minimum context invariant: ensure at least one user message survives.
    bool has_user = false;
    for (const auto& msg : core)
        if (msg.role == "user") { has_user = true; break; }
    if (!has_user && !history.empty()) {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->role == "user") {
                core.push_back(*it);
                break;
            }
        }
    }

    // Prepend the saved system prompt so the classifier cannot remove it.
    if (saved_system.role == "system")
        core.insert(core.begin(), std::move(saved_system));

    // Repair tool_call/tool_result group splits introduced by pruning or
    // misclassification so the rebuilt history stays API-valid.
    sanitize_tool_pairs(core);

    return core;
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
std::vector<Message> enforce_target_budget(std::vector<Message> compressed,
                                           size_t context_size,
                                           const CompressionConfig& cfg) {
    if (context_size == 0) return compressed;
    const int pct = cfg.target_pct > 0 ? cfg.target_pct
                                       : kDefaultCompressionTargetPct;
    const auto target = static_cast<size_t>(
        static_cast<double>(context_size) * static_cast<double>(pct) / 100.0);
    if (target == 0) return compressed;

    size_t used = estimate_tokens(compressed);
    if (used <= target) return compressed;

    // Locate the compressed-context archive message (system role, carries the
    // JSON block we extend). It must exist (apply_classification always emits
    // one); if not, bail — do not fabricate.
    size_t ctx_idx = compressed.size();
    for (size_t i = 0; i < compressed.size(); ++i) {
        if (compressed[i].content.compare(
                0, sizeof(kCompressedContextPrefix) - 1,
                kCompressedContextPrefix) == 0) {
            ctx_idx = i;
            break;
        }
    }
    if (ctx_idx == compressed.size()) return compressed;

    // Protected tail boundary: the last `keep_last_prompts` user messages (and
    // everything after them) stay verbatim — same rule as the classify guard.
    // The compressed-context message is a trailing system message; ignore it.
    const int keep = cfg.keep_last_prompts > 0
                         ? cfg.keep_last_prompts
                         : kDefaultCompressionKeepLastPrompts;
    size_t tail_start = compressed.size();
    {
        std::vector<size_t> users;
        for (size_t i = 1; i < compressed.size(); ++i)
            if (compressed[i].role == "user") users.push_back(i);
        auto protect = static_cast<size_t>(keep);
        if (protect > users.size()) protect = users.size();
        if (protect > 0) tail_start = users[users.size() - protect];
    }

    // Archive eligible core messages oldest-first until under budget. Eligible
    // = before the protected tail, not the system prompt, not the archive msg.
    std::vector<size_t> to_archive;
    for (size_t i = 1; i < tail_start && i < compressed.size(); ++i) {
        if (i == ctx_idx) continue;
        if (compressed[i].role == "system") continue;
        used -= message_tokens(compressed[i]);
        to_archive.push_back(i);
        if (used <= target) break;
    }
    if (to_archive.empty()) return compressed;

    // Remove archived messages (descending keeps indices valid).
    for (auto it = to_archive.rbegin(); it != to_archive.rend(); ++it)
        compressed.erase(compressed.begin() + static_cast<ptrdiff_t>(*it));

    // Re-locate the compressed-context message AFTER the erase (indices
    // shifted left by every removed message that preceded it).
    size_t ctx_idx2 = compressed.size();
    for (size_t i = 0; i < compressed.size(); ++i) {
        if (compressed[i].content.compare(
                0, sizeof(kCompressedContextPrefix) - 1,
                kCompressedContextPrefix) == 0) {
            ctx_idx2 = i;
            break;
        }
    }
    if (ctx_idx2 == compressed.size()) return compressed;

    // Append each archived message to the archive block as a compact entry so
    // the carried summary still reflects what was dropped.
    std::string json_part = compressed[ctx_idx2].content.substr(
        sizeof(kCompressedContextPrefix) - 1);
    json body = json::parse(json_part, nullptr, false);
    if (!body.is_discarded() && body.is_object()) {
        json archive = body.value("archive", json::array());
        for (const size_t idx : to_archive) {
            json entry;
            entry["turns"] = std::to_string(idx);
            entry["summary"] = "(compressed to meet target budget)";
            archive.push_back(std::move(entry));
        }
        body["archive"] = std::move(archive);
        compressed[ctx_idx2].content =
            std::string(kCompressedContextPrefix) + "\n" + body.dump(2);
    }
    return compressed;
}

std::size_t prune_tool_io(std::vector<Message>& history,
                          const CompressionConfig* cfg) {
    // Bulky tool outputs are the largest single token class in agent sessions
    // (multi-KB bash/read results). When the pipeline runs (cfg non-null) we
    // prune them across the WHOLE history INCLUDING the recent tail — the
    // session log retains the original, and sanitize_tool_pairs keeps the
    // message sequence API-valid. With cfg null (direct legacy callers) only
    // messages older than the two-user guard are pruned.
    size_t limit = history.size();  // cfg path: prune everything
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
            // A tool result is only valid directly after an assistant
            // message carrying tool_calls; anything else is an orphan.
            if (out.empty() || out.back().role != "assistant" ||
                out.back().tool_calls.is_null() ||
                !out.back().tool_calls.is_array() ||
                out.back().tool_calls.empty())
                continue;
            out.push_back(m);
            continue;
        }
        out.push_back(m);
        // An assistant tool_calls message whose result was pruned gets a
        // stub result before the next non-tool message.
        if (m.role == "assistant" && !m.tool_calls.is_null() &&
            m.tool_calls.is_array() && !m.tool_calls.empty() &&
            (i + 1 >= history.size() || history[i + 1].role != "tool")) {
            Message stub;
            stub.role = "tool";
            stub.content = kToolOutputOmitted;
            if (m.tool_calls[0].is_object() &&
                m.tool_calls[0].contains("id") &&
                m.tool_calls[0]["id"].is_string())
                stub.tool_call_id = m.tool_calls[0]["id"].get<std::string>();
            out.push_back(std::move(stub));
        }
    }
    history.swap(out);
}

void apply_memory_ops(MemoryStore& store,
                      const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path,
                      std::vector<ExtractionItem>* items) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            int evidence = store.deprecate(op.content);
            if (evidence >= 0 && items) {
                items->push_back(
                    {op.name.empty() ? op.content.substr(0, 40) : op.name,
                     "deprecate", evidence, evidence > 0});
            }
            continue;
        }
        // Validate name uniqueness: if a memory with the same name but
        // different content exists, flag as conflict so the LLM picks
        // a different name rather than silently overwriting.
        std::string label = op.name.empty() ? op.content.substr(0, 40) : op.name;
        const Memory* existing = store.find_memory(label);
        if (existing && existing->content != op.content) {
            if (items) items->push_back({label, "conflict: name \"" + label + "\" already used for different content", existing->evidence_count, existing->promoted});
            continue;
        }
        Memory mem;
        mem.name = label;
        mem.content = op.content;
        mem.tags = op.tags;
        mem.evidence_count = store.memory_promote_threshold();
        mem.promoted = true;
        store.upsert(mem);
        if (items) items->push_back({label, "upsert", mem.evidence_count, true});
    }

    if (!store_path.empty())
        store.save(store_path);
}

void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path,
                     std::vector<ExtractionItem>* items) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            int evidence = store.deprecate(op.content);
            if (evidence >= 0 && items) {
                items->push_back(
                    {op.name.empty() ? op.content.substr(0, 40) : op.name,
                     "deprecate", evidence, evidence > 0});
            }
            continue;
        }
        std::string label = op.name.empty() ? op.content.substr(0, 40) : op.name;
        const Skill* existing = store.find_skill(label);
        if (existing && existing->content != op.content) {
            if (items) items->push_back({label, "conflict: name \"" + label + "\" already used for different content", existing->evidence_count, existing->promoted});
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
        if (items) items->push_back({label, "upsert", sk.evidence_count, true});
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
std::vector<Message> enforce_headroom(std::vector<Message> compressed,
                                      size_t context_size) {
    if (context_size == 0) return compressed;
    auto budget = static_cast<size_t>(
        static_cast<double>(context_size) * 0.75);

    // Drop oldest non-system messages (the compressed-context message is a
    // system role, so the walk never touches it) until the budget fits.
    size_t used = estimate_tokens(compressed);
    if (used <= budget) return compressed;

    std::vector<size_t> dropped;
    for (size_t i = 1; i < compressed.size(); ++i) {
        if (compressed[i].role == "system") continue;
        used -= message_tokens(compressed[i]);
        dropped.push_back(i);
        if (used <= budget) break;
    }
    if (dropped.empty()) return compressed;

    // Remove dropped messages (descending so indices stay valid).
    for (auto it = dropped.rbegin(); it != dropped.rend(); ++it)
        compressed.erase(compressed.begin() + static_cast<ptrdiff_t>(*it));

    // Re-locate the compressed-context message AFTER the erase (indices
    // shifted) and record each dropped message in its archive block.
    size_t ctx_idx = compressed.size();
    for (size_t i = 0; i < compressed.size(); ++i) {
        if (compressed[i].content.compare(
                0, sizeof(kCompressedContextPrefix) - 1,
                kCompressedContextPrefix) == 0) {
            ctx_idx = i;
            break;
        }
    }
    if (ctx_idx == compressed.size()) return compressed;

    std::string json_part =
        compressed[ctx_idx].content.substr(sizeof(kCompressedContextPrefix) - 1);
    json body = json::parse(json_part, nullptr, false);
    if (!body.is_discarded() && body.is_object()) {
        json archive = body.value("archive", json::array());
        for (const size_t idx : dropped) {
            json entry;
            entry["turns"] = std::to_string(idx);
            entry["summary"] = "(over-budget archived)";
            archive.push_back(std::move(entry));
        }
        body["archive"] = std::move(archive);
        compressed[ctx_idx].content =
            std::string(kCompressedContextPrefix) + "\n" + body.dump(2);
    }
    return compressed;
}

} // namespace agent
