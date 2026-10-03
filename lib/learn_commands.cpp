
#include "agent/learn_commands.h"

#include <algorithm>
#include <cstdio>

namespace agent {

namespace {

bool matches_filter(const LearnItem& item, const std::string& filter) {
    if (filter.empty())
        return true;
    if (filter == "memory")
        return item.type == "memory";
    if (filter == "skill")
        return item.type == "skill";
    return item.name.find(filter) != std::string::npos;
}

} // namespace

std::vector<LearnItem> learn_items(const MemoryStore* store, const std::string& filter) {
    std::vector<LearnItem> out;
    if (!store)
        return out;
    for (const auto& m : store->all_memories()) {
        LearnItem it;
        it.id = m.id;
        it.type = "memory";
        it.name = m.name;
        it.evidence = m.evidence_count;
        it.promoted = m.promoted;
        it.turn = m.last_confirm_turn;
        it.score = store->score_of(m);
        if (matches_filter(it, filter))
            out.push_back(std::move(it));
    }
    for (const auto& s : store->all_skills()) {
        LearnItem it;
        it.id = s.id;
        it.type = "skill";
        it.name = s.name;
        it.evidence = s.evidence_count;
        it.promoted = s.promoted;
        it.turn = s.last_confirm_turn;
        it.trigger = s.trigger_phrase;
        it.score = store->score_of(s);
        if (matches_filter(it, filter))
            out.push_back(std::move(it));
    }
    std::sort(out.begin(), out.end(),
              [](const LearnItem& a, const LearnItem& b) { return a.score > b.score; });
    return out;
}

std::vector<std::string> learn_show_lines(const MemoryStore* store, const std::string& filter) {
    if (!store)
        return {"experience store disabled"};
    auto items = learn_items(store, filter);
    if (items.empty())
        return {"(no learned items)"};
    std::vector<std::string> lines;
    lines.reserve(items.size());
    for (const auto& it : items) {
        char score_buf[16];
        std::snprintf(score_buf, sizeof score_buf, "%.2f", it.score);
        std::string line = it.id;
        line += " \u00b7 ";
        line += it.type;
        line += " \u00b7 ";
        line += it.name;
        line += " \u00b7 evidence ";
        line += std::to_string(it.evidence);
        line += " \u00b7 score ";
        line += score_buf;
        line += " \u00b7 ";
        line += it.promoted ? "promoted" : "-";
        line += " \u00b7 turn ";
        line += std::to_string(it.turn);
        if (!it.trigger.empty()) {
            line += " \u00b7 trigger \"";
            line += it.trigger;
            line += "\"";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

namespace {

// The "a, b, c" rendering of a tag list.
std::string join_tags(const std::vector<std::string>& tags) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i)
            out += ", ";
        out += tags[i];
    }
    return out;
}

std::vector<std::string> memory_lines(const MemoryStore& store, const Memory& m) {
    const std::string tags = join_tags(m.tags);
    char score_buf[16];
    std::snprintf(score_buf, sizeof score_buf, "%.2f", store.score_of(m));
    return {
        "id: " + m.id,
        "type: memory",
        "name: " + m.name,
        "content: " + m.content,
        "tags: " + (tags.empty() ? "-" : tags),
        "evidence: " + std::to_string(m.evidence_count),
        "score: " + std::string(score_buf),
        "promoted: " + std::string(m.promoted ? "yes" : "no"),
        "last turn: " + std::to_string(m.last_confirm_turn),
    };
}

std::vector<std::string> skill_lines(const Skill& s) {
    return {
        "id: " + s.id,
        "type: skill",
        "name: " + s.name,
        "content: " + s.content,
        "evidence: " + std::to_string(s.evidence_count),
        "promoted: " + std::string(s.promoted ? "yes" : "no"),
        "last turn: " + std::to_string(s.last_confirm_turn),
        "trigger: " + s.trigger_phrase,
    };
}

} // namespace

std::vector<std::string> learn_inspect_lines(const MemoryStore* store, const std::string& id,
                                             std::string& error) {
    error.clear();
    if (!store) {
        error = "experience store disabled";
        return {};
    }
    for (const auto& m : store->all_memories())
        if (m.id == id)
            return memory_lines(*store, m);
    for (const auto& s : store->all_skills())
        if (s.id == id)
            return skill_lines(s);
    error = "no learned item with id '" + id + "'";
    return {};
}

std::vector<std::string> learn_summary_lines(const MemoryStore* store,
                                             const ExperienceConfig& cfg) {
    size_t mem = 0;
    size_t sk = 0;
    size_t promoted = 0;
    if (store) {
        for (const auto& m : store->all_memories()) {
            ++mem;
            if (m.promoted)
                ++promoted;
        }
        for (const auto& s : store->all_skills()) {
            ++sk;
            if (s.promoted)
                ++promoted;
        }
    }
    std::string line = "memories: ";
    line += std::to_string(mem);
    line += "/";
    line += std::to_string(cfg.max_memories);
    line += " \u00b7 skills: ";
    line += std::to_string(sk);
    line += "/";
    line += std::to_string(cfg.max_skills);
    line += " \u00b7 promoted: ";
    line += std::to_string(promoted);
    line += " \u00b7 path: ";
    line += cfg.store_path.empty() ? "(not configured)" : cfg.store_path;
    return {line};
}

} // namespace agent
