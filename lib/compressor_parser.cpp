// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"

#include <cstdlib>
#include <sstream>

namespace agent {

namespace {

Classification tag_from_string(const std::string& s) {
    if (s == "core") return Classification::core;
    if (s == "prune") return Classification::prune;
    return Classification::context;
}

} // namespace

CompressionResponse parse_compression_response(const std::string& json_str) {
    CompressionResponse cr;

    if (json_str.empty()) return cr;

    try {
        json j = json::parse(json_str);

        // Parse classification segments
        if (j.contains("classification") && j["classification"].is_array()) {
            for (const auto& seg : j["classification"]) {
                ClassifiedSegment cs;
                std::string turns = seg.value("turns", "0-0");
                std::string tag = seg.value("tag", "context");
                std::string summary = seg.value("summary", "");
                cs.tag = tag_from_string(tag);
                cs.summary = summary;

                size_t dash = turns.find('-');
                if (dash != std::string::npos) {
                    cs.turn_start = static_cast<size_t>(
                        std::atol(turns.substr(0, dash).c_str()));
                    cs.turn_end = static_cast<size_t>(
                        std::atol(turns.substr(dash + 1).c_str()));
                }
                cr.segments.push_back(cs);
            }
        }

        // Parse memory ops
        if (j.contains("memories") && j["memories"].is_array()) {
            for (const auto& m : j["memories"]) {
                KnowledgeOp op;
                op.content = m.value("content", "");
                op.action = m.value("action", "upsert");
                if (m.contains("tags") && m["tags"].is_array()) {
                    for (const auto& t : m["tags"])
                        op.tags.push_back(t.get<std::string>());
                }
                if (!op.content.empty())
                    cr.memory_ops.push_back(op);
            }
        }

        // Parse skill ops
        if (j.contains("skills") && j["skills"].is_array()) {
            for (const auto& s : j["skills"]) {
                KnowledgeOp op;
                op.content = s.value("content", "");
                op.action = s.value("action", "upsert");
                op.trigger_phrase = s.value("trigger_phrase", "");
                if (s.contains("tags") && s["tags"].is_array()) {
                    for (const auto& t : s["tags"])
                        op.tags.push_back(t.get<std::string>());
                }
                if (!op.content.empty())
                    cr.skill_ops.push_back(op);
            }
        }
    } catch (const std::exception&) {
        // Invalid JSON — return empty response (no-op)
    }

    return cr;
}

} // namespace agent
