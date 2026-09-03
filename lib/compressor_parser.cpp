
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

// Strip markdown code fences and find the first JSON object from text.
// LLMs commonly wrap JSON in ```json ... ``` or prefix conversational text.
std::string extract_json_block(const std::string& raw) {
    auto attempt = json::parse(raw, nullptr, false);
    if (!attempt.is_discarded()) return raw;

    std::string s = raw;
    auto erase_all = [](std::string& t, const std::string& pat) {
        for (auto p = t.find(pat); p != std::string::npos; p = t.find(pat))
            t.erase(p, pat.size());
    };
    erase_all(s, "```json");
    erase_all(s, "```");
    erase_all(s, "\\n");
    attempt = json::parse(s, nullptr, false);
    if (!attempt.is_discarded()) return s;

    // Try to find a JSON object or array
    auto brace = s.find('{');
    auto bracket = s.find('[');
    size_t start = std::string::npos;
    char close_char = 0;
    if (brace != std::string::npos && (bracket == std::string::npos || brace < bracket)) {
        start = brace;
        close_char = '}';
    } else if (bracket != std::string::npos) {
        start = bracket;
        close_char = ']';
    }
    if (start == std::string::npos) return {};
    s = s.substr(start);
    auto close = s.rfind(close_char);
    if (close == std::string::npos) return {};
    s.resize(close + 1);
    attempt = json::parse(s, nullptr, false);
    if (!attempt.is_discarded()) return s;
    return {};
}

} // namespace

CompressionResponse parse_compression_response(const std::string& json_str) {
    CompressionResponse cr;
    if (json_str.empty()) return cr;

    std::string cleaned = extract_json_block(json_str);
    if (cleaned.empty()) return cr;

    try {
        json j = json::parse(cleaned);

        // If the LLM returned a bare array, it's a classification-only response.
        if (j.is_array()) {
            for (const auto& seg : j) {
                if (!seg.is_object()) continue;
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
            return cr;  // Classification only — no memory/skill ops
        }

        // Parse the work-state summary (top-level string the classifier emits).
        if (j.contains("summary") && j["summary"].is_string())
            cr.summary = j["summary"].get<std::string>();

        // Parse classification segments from an object
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
                op.name = m.value("name", "");
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
                op.name = s.value("name", "");
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
    } catch (const std::exception&) { // NOLINT: invalid JSON from LLM is expected, not exceptional
    }

    return cr;
}

} // namespace agent
