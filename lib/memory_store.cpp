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
    std::hash<std::string> hasher;
    return std::to_string(hasher(content));
}

double compute_relevance(const std::string& user_message,
                          const std::vector<std::string>& tags) {
    if (user_message.empty() || tags.empty()) return 0.0;
    double score = 0.0;
    for (const auto& tag : tags) {
        if (user_message.find(tag) != std::string::npos)
            score += 1.0;
    }
    return score / static_cast<double>(tags.size());
}

double compute_freshness(int last_confirm_turn, int current_turn) {
    if (last_confirm_turn <= 0 || current_turn <= 0) return 0.0;
    int age = current_turn - last_confirm_turn;
    if (age < 0) return 0.0;
    if (age > 20) return 0.0;
    return 1.0 - (static_cast<double>(age) / 20.0);
}

double compute_score(const Memory& mem, const std::string& user_msg,
                     int current_turn) {
    double evidence_w = 0.5;
    double relevance_w = 0.3;
    double freshness_w = 0.2;
    double rel = compute_relevance(user_msg, mem.tags);
    double fresh = compute_freshness(mem.last_confirm_turn, current_turn);
    return (mem.evidence_count * evidence_w) + (rel * relevance_w) +
           (fresh * freshness_w);
}

json memory_to_json(const Memory& mem) {
    return {
        {"id", mem.id},
        {"content", mem.content},
        {"tags", mem.tags},
        {"evidence", mem.evidence_count},
        {"last_confirm_turn", mem.last_confirm_turn},
        {"score", mem.score},
        {"promoted", mem.promoted}
    };
}

Memory json_to_memory(const json& j) {
    Memory mem;
    mem.id = j.value("id", "");
    mem.content = j.value("content", "");
    for (const auto& t : j.value("tags", json::array()))
        mem.tags.push_back(t.get<std::string>());
    mem.evidence_count = j.value("evidence", 0);
    mem.last_confirm_turn = j.value("last_confirm_turn", 0);
    mem.score = j.value("score", 0.0);
    mem.promoted = j.value("promoted", false);
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
        {"last_confirm_turn", sk.last_confirm_turn},
        {"score", sk.score},
        {"promoted", sk.promoted}
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
    sk.last_confirm_turn = j.value("last_confirm_turn", 0);
    sk.score = j.value("score", 0.0);
    sk.promoted = j.value("promoted", false);
    return sk;
}

} // namespace

// =========================================================================
// JsonMemoryStore  —  file-backed persistence with evidence staging
// =========================================================================

class JsonMemoryStore : public MemoryStore {
public:
    explicit JsonMemoryStore(ExperienceConfig cfg)
        : cfg_(std::move(cfg)) {
        if (!cfg_.store_path.empty())
            load(cfg_.store_path);
    }

    void set_current_turn(size_t turn) override {
        current_turn_ = static_cast<int>(turn);
    }

    void upsert(const Memory& memory) override {
        std::string key = memory.id.empty() ? hash_content(memory.content) : memory.id;

        auto it = memories_.find(key);
        if (it != memories_.end()) {
            // Re-confirmation: increment evidence, update turn
            Memory& existing = it->second;
            existing.evidence_count = std::min(
                cfg_.memory_promote_threshold * 3,
                existing.evidence_count + 1);
            existing.last_confirm_turn = current_turn_;
            existing.tags = memory.tags;
            // Promote if threshold reached
            if (!existing.promoted &&
                existing.evidence_count >= cfg_.memory_promote_threshold) {
                existing.promoted = true;
            }
        } else {
            Memory m = memory;
            m.id = key;
            if (m.evidence_count <= 0) m.evidence_count = 1;
            m.last_confirm_turn = current_turn_;
            memories_[key] = m;
        }
    }

    void upsert(const Skill& skill) override {
        std::string key = skill.id.empty() ? hash_content(skill.content) : skill.id;

        auto it = skills_.find(key);
        if (it != skills_.end()) {
            Skill& existing = it->second;
            existing.evidence_count = std::min(
                cfg_.skill_promote_threshold * 3,
                existing.evidence_count + 1);
            existing.last_confirm_turn = current_turn_;
            existing.tags = skill.tags;
            if (!skill.trigger_phrase.empty())
                existing.trigger_phrase = skill.trigger_phrase;
            if (!existing.promoted &&
                existing.evidence_count >= cfg_.skill_promote_threshold) {
                existing.promoted = true;
            }
        } else {
            Skill sk = skill;
            sk.id = key;
            if (sk.evidence_count <= 0) sk.evidence_count = 1;
            sk.last_confirm_turn = current_turn_;
            skills_[key] = sk;
        }
    }

    std::vector<Memory> top_memories(
        size_t k, const std::string& user_message) const override {
        std::vector<std::pair<double, Memory>> scored;
        scored.reserve(memories_.size());
        for (const auto& [id, mem] : memories_) {
            if (!mem.promoted) continue;
            double s = compute_score(mem, user_message, current_turn_);
            Memory copy = mem;
            copy.score = s;
            scored.emplace_back(s, std::move(copy));
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) {
                      return a.first > b.first;
                  });
        std::vector<Memory> result;
        for (size_t i = 0; i < std::min(k, scored.size()); ++i)
            result.push_back(std::move(scored[i].second));
        return result;
    }

    std::vector<Skill> top_skills(
        size_t k, const std::string& user_message) const override {
        std::vector<Skill> filtered;
        for (const auto& [id, sk] : skills_) {
            if (!sk.promoted) continue;
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
            if (mem.evidence_count > 0)
                mem.evidence_count -= 1;
            if (mem.evidence_count <= 0)
                mem.promoted = false;
        }
        for (auto& [id, sk] : skills_) {
            if (sk.evidence_count > 0)
                sk.evidence_count -= 1;
            if (sk.evidence_count <= 0)
                sk.promoted = false;
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
    int current_turn_ = 0;
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
