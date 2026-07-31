
#include "agent/compressor.h"
#include "agent/experience.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace agent {

std::vector<Message> apply_classification(
    const std::vector<Message>& history,
    const CompressionResponse& response) {
    if (history.empty()) return history;
    if (response.segments.empty()) return history;

    // Build per-turn tag array from segments
    std::vector<Classification> tags(history.size(), Classification::core);
    std::vector<std::string> summaries(history.size());

    for (const auto& seg : response.segments) {
        size_t start = std::min(seg.turn_start, history.size() - 1);
        size_t end = std::min(seg.turn_end, history.size() - 1);
        for (size_t i = start; i <= end; ++i) {
            tags[i] = seg.tag;
            summaries[i] = seg.summary;
        }
    }

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

    size_t start_idx = saved_system.role == "system" ? 1 : 0;
    for (size_t i = start_idx; i < history.size(); ++i) {
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

    // Build archive JSON
    json archive_json = json::array();
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
    compressed_msg.content = "Compressed conversation context:\n" + ctx.dump(2);
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

    return core;
}

namespace {

std::string hash_content(const std::string& content) {
    std::hash<std::string> hasher;
    return std::to_string(hasher(content));
}

} // namespace

void apply_memory_ops(MemoryStore& store,
                      const std::vector<KnowledgeOp>& ops,
                      const std::string& store_path,
                      std::vector<ExtractionItem>* items,
                      int promote_threshold) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            auto existing = store.top_memories(100, "");
            std::string key = hash_content(op.content);
            for (const auto& mem : existing) {
                if (mem.id == key || mem.content == op.content) {
                    Memory updated = mem;
                    updated.evidence_count = std::max(0, mem.evidence_count - 1);
                    store.upsert(updated);
                    if (items) items->push_back({op.name.empty() ? op.content.substr(0, 40) : op.name, "deprecate", updated.evidence_count, updated.promoted});
                    break;
                }
            }
        } else {
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
            mem.evidence_count = promote_threshold;
            mem.promoted = true;
            store.upsert(mem);
            if (items) items->push_back({label, "upsert", promote_threshold, true});
        }
    }

    if (!store_path.empty())
        store.save(store_path);
}

void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path,
                     std::vector<ExtractionItem>* items,
                     int promote_threshold) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            auto existing = store.top_skills(100, "");
            std::string key = hash_content(op.content);
            for (const auto& sk : existing) {
                if (sk.id == key || sk.content == op.content) {
                    Skill updated = sk;
                    updated.evidence_count = std::max(0, sk.evidence_count - 1);
                    store.upsert(updated);
                    if (items) items->push_back({op.name.empty() ? op.content.substr(0, 40) : op.name, "deprecate", updated.evidence_count, updated.promoted});
                    break;
                }
            }
        } else {
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
            sk.evidence_count = promote_threshold;
            sk.promoted = true;
            store.upsert(sk);
            if (items) items->push_back({label, "upsert", promote_threshold, true});
        }
    }

    if (!store_path.empty())
        store.save(store_path);
}

// ---------------------------------------------------------------------------
// Budget enforcement
// ---------------------------------------------------------------------------

namespace {

// Recount estimated token count for a message vector.
size_t count_tokens(const std::vector<Message>& msgs) {
    size_t n = 0;
    for (const auto& msg : msgs) {
        n += msg.content.size() / 4;
        n += msg.reasoning.size() / 4;
        if (!msg.tool_calls.is_null())
            n += msg.tool_calls.dump().size() / 4;
        n += 4;
    }
    return n;
}

} // namespace

// Enforce that the compressed context leaves at least 25% headroom.
std::vector<Message> enforce_headroom(std::vector<Message> compressed,
                                       size_t context_size) {
    if (context_size == 0) return compressed;
    auto budget = static_cast<size_t>(
        static_cast<double>(context_size) * 0.75);
    size_t used = count_tokens(compressed);
    if (used <= budget) return compressed;

    for (size_t i = 1; i < compressed.size(); ++i) {
        if (compressed[i].role == "system") continue;
        Message archived;
        archived.role = "system";
        archived.content = "[over-budget archived]";
        compressed[i] = std::move(archived);

        used = count_tokens(compressed);
        if (used <= budget) break;
    }
    return compressed;
}

} // namespace agent
