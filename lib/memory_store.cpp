// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/experience.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

std::string hash_content(const std::string& content) {
    // Simple content hash for dedup (not cryptographic).
    std::hash<std::string> hasher;
    return std::to_string(hasher(content));
}

double compute_relevance(const std::string& user_message,
                          const std::vector<std::string>& tags) {
    if (user_message.empty() || tags.empty()) return 0.0;
    double score = 0.0;
    for (const auto& tag : tags) {
        if (user_message.find(tag) != std::string::npos) {
            score += 1.0;
        }
    }
    return score / static_cast<double>(tags.size());
}

json memory_to_json(const Memory& mem) {
    return {
        {"id", mem.id},
        {"content", mem.content},
        {"tags", mem.tags},
        {"evidence", mem.evidence_count},
        {"score", mem.score}
    };
}

Memory json_to_memory(const json& j) {
    Memory mem;
    mem.id = j.value("id", "");
    mem.content = j.value("content", "");
    for (const auto& t : j.value("tags", json::array()))
        mem.tags.push_back(t.get<std::string>());
    mem.evidence_count = j.value("evidence", 0);
    mem.score = j.value("score", 0.0);
    return mem;
}

json skill_to_json(const Skill& sk) {
    return {
        {"id", sk.id},
        {"content", sk.content},
        {"trigger_phrase", sk.trigger_phrase},
        {"steps", sk.steps},
        {"expected_outcome", sk.expected_outcome},
        {"tags", sk.tags},
        {"evidence", sk.evidence_count},
        {"score", sk.score}
    };
}

Skill json_to_skill(const json& j) {
    Skill sk;
    sk.id = j.value("id", "");
    sk.content = j.value("content", "");
    sk.trigger_phrase = j.value("trigger_phrase", "");
    for (const auto& s : j.value("steps", json::array()))
        sk.steps.push_back(s.get<std::string>());
    sk.expected_outcome = j.value("expected_outcome", "");
    for (const auto& t : j.value("tags", json::array()))
        sk.tags.push_back(t.get<std::string>());
    sk.evidence_count = j.value("evidence", 0);
    sk.score = j.value("score", 0.0);
    return sk;
}

} // namespace

// =========================================================================
// JsonMemoryStore  —  file-backed persistence
// =========================================================================

class JsonMemoryStore : public MemoryStore {
public:
    explicit JsonMemoryStore(ExperienceConfig cfg)
        : cfg_(std::move(cfg)) {
        if (!cfg_.store_path.empty())
            load(cfg_.store_path);  // NOLINT: safe, no subclasses override
    }

    void upsert(const Memory& memory) override {
        std::string key = memory.id.empty() ? hash_content(memory.content) : memory.id;
        Memory m = memory;
        m.id = key;
        memories_[key] = m;
    }

    void upsert(const Skill& skill) override {
        std::string key = skill.id.empty() ? hash_content(skill.content) : skill.id;
        Skill sk = skill;
        sk.id = key;
        skills_[key] = sk;
    }

    std::vector<Memory> top_memories(
        size_t k, const std::string& user_message) const override {
        // Score + sort + take top K
        std::vector<std::pair<double, Memory>> scored;
        scored.reserve(memories_.size());
        for (const auto& [id, mem] : memories_) {
            double rel = compute_relevance(user_message, mem.tags);
            double score = (mem.evidence_count * 0.5) + (rel * 0.3)
                           + 0.2;  // freshness placeholder
            scored.emplace_back(score, mem);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) {
                      return a.first > b.first;
                  });
        std::vector<Memory> result;
        for (size_t i = 0; i < std::min(k, scored.size()); ++i)
            result.push_back(scored[i].second);
        return result;
    }

    std::vector<Skill> top_skills(
        size_t k, const std::string& user_message) const override {
        std::vector<Skill> filtered;
        for (const auto& [id, sk] : skills_) {
            if (sk.trigger_phrase.empty() ||
                user_message.find(sk.trigger_phrase) != std::string::npos) {
                filtered.push_back(sk);
            }
        }
        std::sort(filtered.begin(), filtered.end(),
                  [](const Skill& a, const Skill& b) {
                      return a.evidence_count > b.evidence_count;
                  });
        if (filtered.size() > k)
            filtered.resize(k);
        return filtered;
    }

    void decay_all() override {
        for (auto& [id, mem] : memories_) {
            mem.evidence_count = std::max(0, mem.evidence_count - 1);
        }
        for (auto& [id, sk] : skills_) {
            sk.evidence_count = std::max(0, sk.evidence_count - 1);
        }
    }

    bool load(const std::string& path) override {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        try {
            json j;
            f >> j;
            for (const auto& m : j.value("memories", json::array())) {
                Memory mem = json_to_memory(m);
                memories_[mem.id] = mem;
            }
            for (const auto& s : j.value("skills", json::array())) {
                Skill sk = json_to_skill(s);
                skills_[sk.id] = sk;
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    bool save(const std::string& path) const override {
        json j;
        j["version"] = 1;
        json mems = json::array();
        for (const auto& [id, mem] : memories_)
            mems.push_back(memory_to_json(mem));
        j["memories"] = mems;
        json sks = json::array();
        for (const auto& [id, sk] : skills_)
            sks.push_back(skill_to_json(sk));
        j["skills"] = sks;
        std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp);
            if (!f.is_open()) return false;
            f << j.dump(2);
        }
        std::rename(tmp.c_str(), path.c_str());
        return true;
    }

private:
    ExperienceConfig cfg_;
    std::unordered_map<std::string, Memory> memories_;
    std::unordered_map<std::string, Skill> skills_;
};

// =========================================================================
// Factory
// =========================================================================

std::unique_ptr<MemoryStore> make_memory_store(const ExperienceConfig& cfg) {
    return std::make_unique<JsonMemoryStore>(cfg);
}

ExperienceConfig load_experience_config(const Config& cfg) {
    ExperienceConfig ec;
    if (!cfg.experience_enabled)
        ec.enabled = false;
    if (cfg.experience_max_memories > 0)
        ec.max_memories = static_cast<size_t>(cfg.experience_max_memories);
    if (cfg.experience_max_skills > 0)
        ec.max_skills = static_cast<size_t>(cfg.experience_max_skills);
    if (ec.store_path.empty()) {
        const char* home = std::getenv("HOME");
        if (home) ec.store_path = std::string(home) + "/.amber/memories.json";
    }
    return ec;
}

} // namespace agent
