// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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
        size_t end = std::min(seg.turn_end, history.size() - 1);
        for (size_t i = seg.turn_start; i <= end; ++i) {
            tags[i] = seg.tag;
            summaries[i] = seg.summary;
        }
    }

    // Separate messages by tag
    std::vector<Message> core;
    std::vector<std::pair<size_t, std::string>> archive_segments;
    size_t prune_count = 0;

    for (size_t i = 0; i < history.size(); ++i) {
        switch (tags[i]) {
            case Classification::core:
                core.push_back(history[i]);
                break;
            case Classification::context:
                if (archive_segments.empty() ||
                    archive_segments.back().first != i - 1) {
                    archive_segments.push_back({i, summaries[i]});
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
        entry["turns"] = std::to_string(seg.first) + "-" +
                         std::to_string(seg.first);
        entry["summary"] = seg.second.empty() ? "(compressed)" : seg.second;
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
                      const std::string& store_path) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            // Find existing memory by content hash and deprecate
            auto existing = store.top_memories(100, "");
            std::string key = hash_content(op.content);
            for (const auto& mem : existing) {
                if (mem.id == key || mem.content == op.content) {
                    Memory updated = mem;
                    updated.evidence_count = std::max(0, mem.evidence_count - 1);
                    store.upsert(updated);
                    break;
                }
            }
        } else {
            // Upsert — LLM explicitly confirmed this knowledge; promote
            // immediately so it appears in the system prompt on the next turn.
            Memory mem;
            mem.content = op.content;
            mem.tags = op.tags;
            mem.evidence_count = 3;
            mem.promoted = true;
            store.upsert(mem);
        }
    }

    if (!store_path.empty())
        store.save(store_path);
}

void apply_skill_ops(MemoryStore& store,
                     const std::vector<KnowledgeOp>& ops,
                     const std::string& store_path) {
    for (const auto& op : ops) {
        if (op.action == "deprecate") {
            auto existing = store.top_skills(100, "");
            std::string key = hash_content(op.content);
            for (const auto& sk : existing) {
                if (sk.id == key || sk.content == op.content) {
                    Skill updated = sk;
                    updated.evidence_count = std::max(0, sk.evidence_count - 1);
                    store.upsert(updated);
                    break;
                }
            }
        } else {
            Skill sk;
            sk.content = op.content;
            sk.tags = op.tags;
            sk.trigger_phrase = op.trigger_phrase;
            sk.evidence_count = 3;
            sk.promoted = true;
            store.upsert(sk);
        }
    }

    if (!store_path.empty())
        store.save(store_path);
}

CompressionResult build_compression_result(
    const std::vector<Message>& before,
    const std::vector<Message>& after,
    const std::vector<Classification>& per_turn_tags) {
    CompressionResult r;
    r.messages_before = before.size();
    r.messages_after = after.size();

    for (const auto& msg : before)
        r.tokens_before += (msg.content.size() + msg.reasoning.size()) / 4;
    for (const auto& msg : after)
        r.tokens_after += (msg.content.size() + msg.reasoning.size()) / 4;

    for (auto t : per_turn_tags) {
        if (t == Classification::core) ++r.core_count;
        else if (t == Classification::context) ++r.context_count;
        else if (t == Classification::prune) ++r.prune_count;
    }

    return r;
}

} // namespace agent
