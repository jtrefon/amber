#include "memory_helpers.h"

#include <algorithm>
#include <cmath>

namespace agent {
namespace memory_detail {

std::string hash_content(const std::string& content) {
    std::hash<std::string> hasher;
    return std::to_string(hasher(content));
}

double compute_relevance(const std::string& user_message, const std::vector<std::string>& tags) {
    if (user_message.empty() || tags.empty())
        return 0.0;
    double score = 0.0;
    for (const auto& tag : tags) {
        if (user_message.find(tag) != std::string::npos)
            score += 1.0;
    }
    return score / static_cast<double>(tags.size());
}

double compute_freshness(int last_confirm_turn, int current_turn) {
    if (last_confirm_turn <= 0 || current_turn <= 0)
        return 0.0;
    int age = current_turn - last_confirm_turn;
    if (age < 0)
        return 0.0;
    if (age > 20)
        return 0.0;
    return 1.0 - (static_cast<double>(age) / 20.0);
}

double compute_score(const KnowledgeItem& mem, const std::string& user_msg, int current_turn) {
    double evidence_w = 0.5;
    double relevance_w = 0.3;
    double freshness_w = 0.2;
    double rel = compute_relevance(user_msg, mem.tags);
    double fresh = compute_freshness(mem.last_confirm_turn, current_turn);
    return (mem.evidence_count * evidence_w) + (rel * relevance_w) + (fresh * freshness_w);
}

void upsert_memory(std::unordered_map<std::string, Memory>& memories, const Memory& memory,
                   const ExperienceConfig& cfg, int current_turn) {
    std::string key = memory.id.empty() ? hash_content(memory.content) : memory.id;
    auto it = memories.find(key);
    if (it != memories.end()) {
        Memory& existing = it->second;
        existing.evidence_count =
            std::min(cfg.memory_promote_threshold * 3, existing.evidence_count + 1);
        existing.last_confirm_turn = current_turn;
        existing.tags = memory.tags;
        if (!existing.promoted && existing.evidence_count >= cfg.memory_promote_threshold) {
            existing.promoted = true;
        }
    } else {
        Memory m = memory;
        m.id = key;
        if (m.evidence_count <= 0)
            m.evidence_count = 1;
        m.last_confirm_turn = current_turn;
        memories[key] = m;
    }
}

void upsert_skill(std::unordered_map<std::string, Skill>& skills, const Skill& skill,
                  const ExperienceConfig& cfg, int current_turn) {
    std::string key = skill.id.empty() ? hash_content(skill.content) : skill.id;
    auto it = skills.find(key);
    if (it != skills.end()) {
        Skill& existing = it->second;
        existing.evidence_count =
            std::min(cfg.skill_promote_threshold * 3, existing.evidence_count + 1);
        existing.last_confirm_turn = current_turn;
        existing.tags = skill.tags;
        if (!skill.trigger_phrase.empty())
            existing.trigger_phrase = skill.trigger_phrase;
        if (!existing.promoted && existing.evidence_count >= cfg.skill_promote_threshold) {
            existing.promoted = true;
        }
    } else {
        Skill sk = skill;
        sk.id = key;
        if (sk.evidence_count <= 0)
            sk.evidence_count = 1;
        sk.last_confirm_turn = current_turn;
        skills[key] = sk;
    }
}

namespace {

// Ties are broken by name, then id. Without this the block order follows
// unordered_map iteration, so a rebuild can reorder the [system][memory]
// prefix even when no knowledge changed.
bool name_before(const KnowledgeItem& a, const KnowledgeItem& b) {
    if (a.name != b.name)
        return a.name < b.name;
    return a.id < b.id;
}

} // namespace

std::vector<Memory> select_top_memories(const std::unordered_map<std::string, Memory>& memories,
                                        size_t k, const std::string& user_message,
                                        int current_turn) {
    std::vector<std::pair<double, Memory>> scored;
    scored.reserve(memories.size());
    for (const auto& [id, mem] : memories) {
        if (!mem.promoted)
            continue;
        double s = compute_score(mem, user_message, current_turn);
        Memory copy = mem;
        copy.score = s;
        scored.emplace_back(s, std::move(copy));
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return name_before(a.second, b.second);
    });
    std::vector<Memory> result;
    for (size_t i = 0; i < std::min(k, scored.size()); ++i)
        result.push_back(std::move(scored[i].second));
    return result;
}

std::vector<Skill> select_top_skills(const std::unordered_map<std::string, Skill>& skills, size_t k,
                                     const std::string& user_message) {
    std::vector<Skill> filtered;
    for (const auto& [id, sk] : skills) {
        if (!sk.promoted)
            continue;
        if (sk.trigger_phrase.empty() ||
            user_message.find(sk.trigger_phrase) != std::string::npos) {
            filtered.push_back(sk);
        }
    }
    std::sort(filtered.begin(), filtered.end(), [](const Skill& a, const Skill& b) {
        if (a.evidence_count != b.evidence_count)
            return a.evidence_count > b.evidence_count;
        return name_before(a, b);
    });
    if (filtered.size() > k)
        filtered.resize(k);
    return filtered;
}

std::vector<Memory> sorted_memories(const std::unordered_map<std::string, Memory>& memories,
                                    int current_turn) {
    std::vector<Memory> out;
    out.reserve(memories.size());
    for (const auto& [id, mem] : memories)
        out.push_back(mem);
    std::sort(out.begin(), out.end(), [&](const Memory& a, const Memory& b) {
        const double a_score = compute_score(a, "", current_turn);
        const double b_score = compute_score(b, "", current_turn);
        if (a_score != b_score)
            return a_score > b_score;
        return name_before(a, b);
    });
    return out;
}

std::vector<Skill> sorted_skills(const std::unordered_map<std::string, Skill>& skills,
                                 int current_turn) {
    std::vector<Skill> out;
    out.reserve(skills.size());
    for (const auto& [id, sk] : skills)
        out.push_back(sk);
    std::sort(out.begin(), out.end(), [&](const Skill& a, const Skill& b) {
        const double a_score = compute_score(a, "", current_turn);
        const double b_score = compute_score(b, "", current_turn);
        if (a_score != b_score)
            return a_score > b_score;
        return name_before(a, b);
    });
    return out;
}

namespace {

template <typename T>
void decay_map(std::unordered_map<std::string, T>& items, const ExperienceConfig& cfg,
               int current_turn) {
    for (auto it = items.begin(); it != items.end();) {
        T& item = it->second;
        if (item.evidence_count <= 0) {
            it = items.erase(it);
            continue;
        }
        // An item confirmed in this cycle was just reconfirmed. Decay is the
        // signal that knowledge was NOT reconfirmed, so it cannot apply yet.
        if (current_turn > 0 && item.last_confirm_turn == current_turn) {
            ++it;
            continue;
        }
        const int decay = std::max(1, static_cast<int>(item.evidence_count * cfg.decay_rate));
        item.evidence_count -= decay;
        if (item.evidence_count <= 0)
            item.promoted = false;
        ++it;
    }
}

} // namespace

void decay_maps(std::unordered_map<std::string, Memory>& memories,
                std::unordered_map<std::string, Skill>& skills, const ExperienceConfig& cfg,
                int current_turn) {
    decay_map(memories, cfg, current_turn);
    decay_map(skills, cfg, current_turn);
}

} // namespace memory_detail
} // namespace agent
