#pragma once

#include "agent/experience.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {
namespace memory_detail {

std::string hash_content(const std::string& content);
double compute_relevance(const std::string& user_message, const std::vector<std::string>& tags);
double compute_freshness(int last_confirm_turn, int current_turn);
double compute_score(const KnowledgeItem& mem, const std::string& user_msg, int current_turn);

nlohmann::json memory_to_json(const Memory& mem);
Memory json_to_memory(const nlohmann::json& j);
nlohmann::json skill_to_json(const Skill& sk);
Skill json_to_skill(const nlohmann::json& j);

void upsert_memory(std::unordered_map<std::string, Memory>& memories, const Memory& memory,
                   const ExperienceConfig& cfg, int current_turn);
void upsert_skill(std::unordered_map<std::string, Skill>& skills, const Skill& skill,
                  const ExperienceConfig& cfg, int current_turn);
std::vector<Memory> select_top_memories(const std::unordered_map<std::string, Memory>& memories,
                                        size_t k, const std::string& user_message, int current_turn);
std::vector<Skill> select_top_skills(const std::unordered_map<std::string, Skill>& skills,
                                     size_t k, const std::string& user_message);
std::vector<Memory> sorted_memories(const std::unordered_map<std::string, Memory>& memories,
                                    int current_turn);
std::vector<Skill> sorted_skills(const std::unordered_map<std::string, Skill>& skills,
                                 int current_turn);
void decay_maps(std::unordered_map<std::string, Memory>& memories,
                std::unordered_map<std::string, Skill>& skills, const ExperienceConfig& cfg);
bool load_store(const std::string& path, std::unordered_map<std::string, Memory>& memories,
                std::unordered_map<std::string, Skill>& skills);
bool save_store(const std::string& path, const std::unordered_map<std::string, Memory>& memories,
                const std::unordered_map<std::string, Skill>& skills);

template <typename T>
int deprecate_one_map(const std::string& content, std::unordered_map<std::string, T>& items) {
    std::string key = hash_content(content);
    auto it = items.find(key);
    if (it == items.end()) {
        for (auto& [id, item] : items) {
            if (item.content == content) {
                it = items.find(id);
                break;
            }
        }
    }
    if (it == items.end()) return -1;
    T& item = it->second;
    item.evidence_count = std::max(0, item.evidence_count - 1);
    if (item.evidence_count == 0) {
        items.erase(it);
        return 0;
    }
    return item.evidence_count;
}

} // namespace memory_detail
} // namespace agent
