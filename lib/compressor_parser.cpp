
#include "agent/compressor.h"

#include <cstdlib>
#include <sstream>

namespace agent {

namespace {

Classification tag_from_string(const std::string& s) {
    if (s == "core")
        return Classification::core;
    if (s == "prune")
        return Classification::prune;
    return Classification::context;
}

// Strip markdown code fences and find the first JSON object from text.
// LLMs commonly wrap JSON in ```json ... ``` or prefix conversational text.
namespace {

bool parses(const std::string& text) {
    return !json::parse(text, nullptr, false).is_discarded();
}

void erase_all(std::string& t, const std::string& pat) {
    for (auto p = t.find(pat); p != std::string::npos; p = t.find(pat))
        t.erase(p, pat.size());
}

// The prose a model wraps JSON in: code fences and escaped newlines removed.
std::string strip_fences(const std::string& raw) {
    std::string s = raw;
    erase_all(s, "```json");
    erase_all(s, "```");
    erase_all(s, "\\n");
    return s;
}

// Cut from the first '{' or '[' to the last matching close, so trailing prose
// after the object does not defeat the parse. Returns {} when there is no
// candidate, or no closing brace for the opener that was found.
std::string slice_outermost(const std::string& s) {
    const auto brace = s.find('{');
    const auto bracket = s.find('[');
    size_t start = std::string::npos;
    char close_char = 0;
    if (brace != std::string::npos && (bracket == std::string::npos || brace < bracket)) {
        start = brace;
        close_char = '}';
    } else if (bracket != std::string::npos) {
        start = bracket;
        close_char = ']';
    }
    if (start == std::string::npos)
        return {};
    std::string body = s.substr(start);
    const auto close = body.rfind(close_char);
    if (close == std::string::npos)
        return {};
    body.resize(close + 1);
    return body;
}

} // namespace

// Recover a JSON object from a model response, trying progressively looser
// readings of the text. The stages are named; the sequence is the whole contract.
std::string extract_json_block(const std::string& raw) {
    if (parses(raw))
        return raw;
    std::string s = strip_fences(raw);
    if (parses(s))
        return s;
    s = slice_outermost(s);
    return parses(s) ? s : std::string{};
}

} // namespace

// "start-end" → (start, end). A missing dash leaves both at 0.
void parse_turn_range(const std::string& turns, ClassifiedSegment& cs) {
    const size_t dash = turns.find('-');
    if (dash == std::string::npos)
        return;
    cs.turn_start = static_cast<size_t>(std::atol(turns.substr(0, dash).c_str()));
    cs.turn_end = static_cast<size_t>(std::atol(turns.substr(dash + 1).c_str()));
}

ClassifiedSegment parse_segment(const json& seg) {
    ClassifiedSegment cs;
    cs.tag = tag_from_string(seg.value("tag", "context"));
    cs.summary = seg.value("summary", "");
    parse_turn_range(seg.value("turns", "0-0"), cs);
    return cs;
}

// A memory or skill op. `is_skill` adds the trigger phrase.
KnowledgeOp parse_knowledge_op(const json& obj, bool is_skill) {
    KnowledgeOp op;
    op.name = obj.value("name", "");
    op.content = obj.value("content", "");
    op.action = obj.value("action", "upsert");
    if (is_skill)
        op.trigger_phrase = obj.value("trigger_phrase", "");
    if (obj.contains("tags") && obj["tags"].is_array())
        for (const auto& t : obj["tags"])
            op.tags.push_back(t.get<std::string>());
    return op;
}

// Only ops with content are kept: an empty upsert would erase the store entry.
std::vector<KnowledgeOp> parse_knowledge_ops(const json& arr, bool is_skill) {
    std::vector<KnowledgeOp> out;
    for (const auto& obj : arr) {
        KnowledgeOp op = parse_knowledge_op(obj, is_skill);
        if (!op.content.empty())
            out.push_back(std::move(op));
    }
    return out;
}

// The string elements of an array field, empty when the field is absent.
std::vector<std::string> string_list(const json& obj, const char* key) {
    std::vector<std::string> out;
    if (!obj.contains(key) || !obj[key].is_array())
        return out;
    for (const auto& v : obj[key])
        if (v.is_string())
            out.push_back(v.get<std::string>());
    return out;
}

// Session brief (non-fatal: an absent/malformed brief does not affect
// memories/skills — the store retains its last good state). Returns false when
// every field is empty, which means there is nothing to store.
bool parse_brief(const json& b, SessionBrief& brief) {
    brief.intent = b.value("intent", "");
    brief.direction = b.value("direction", "");
    brief.earlier = b.value("earlier", "");
    brief.next = b.value("next", "");
    brief.done = string_list(b, "done");
    brief.avoid = string_list(b, "avoid");
    return !brief.intent.empty() || !brief.direction.empty() || !brief.done.empty() ||
           !brief.next.empty() || !brief.avoid.empty();
}

// The classification, memory, skill and brief blocks of an object response.
void parse_response_body(const json& j, CompressionResponse& cr) {
    // Parse the work-state summary (top-level string the classifier emits).
    if (j.contains("summary") && j["summary"].is_string())
        cr.summary = j["summary"].get<std::string>();

    // Parse classification segments from an object
    if (j.contains("classification") && j["classification"].is_array())
        for (const auto& seg : j["classification"])
            cr.segments.push_back(parse_segment(seg));

    // Parse memory ops
    if (j.contains("memories") && j["memories"].is_array())
        cr.memory_ops = parse_knowledge_ops(j["memories"], /*is_skill=*/false);

    // Parse skill ops
    if (j.contains("skills") && j["skills"].is_array())
        cr.skill_ops = parse_knowledge_ops(j["skills"], /*is_skill=*/true);

    // Parse session brief (non-fatal: absent/malformed brief does not affect
    // memories/skills — the store retains its last good state).
    if (j.contains("brief") && j["brief"].is_object()) {
        SessionBrief brief;
        if (parse_brief(j["brief"], brief))
            cr.brief = std::move(brief);
    }
}

CompressionResponse parse_compression_response(const std::string& json_str) {
    CompressionResponse cr;
    if (json_str.empty())
        return cr;

    std::string cleaned = extract_json_block(json_str);
    if (cleaned.empty())
        return cr;

    try {
        json j = json::parse(cleaned);

        // If the LLM returned a bare array, it's a classification-only response.
        if (j.is_array()) {
            for (const auto& seg : j)
                if (seg.is_object())
                    cr.segments.push_back(parse_segment(seg));
            return cr; // Classification only — no memory/skill ops
        }

        parse_response_body(j, cr);
    } catch (const std::exception&) { // NOLINT: invalid JSON from LLM is expected, not exceptional
    }

    return cr;
}

} // namespace agent
