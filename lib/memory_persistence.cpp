#include "memory_helpers.h"

#include <filesystem>
#include <fstream>

namespace agent {
namespace memory_detail {

nlohmann::json memory_to_json(const Memory& mem) {
    return {
        {"id", mem.id},
        {"name", mem.name},
        {"content", mem.content},
        {"tags", mem.tags},
        {"evidence", mem.evidence_count},
        {"last_confirm_turn", mem.last_confirm_turn},
        {"score", mem.score},
        {"promoted", mem.promoted}};
}

Memory json_to_memory(const nlohmann::json& j) {
    Memory mem;
    mem.id = j.value("id", "");
    mem.name = j.value("name", "");
    mem.content = j.value("content", "");
    for (const auto& t : j.value("tags", nlohmann::json::array())) mem.tags.push_back(t.get<std::string>());
    mem.evidence_count = j.value("evidence", 0);
    mem.last_confirm_turn = j.value("last_confirm_turn", 0);
    mem.score = j.value("score", 0.0);
    mem.promoted = j.value("promoted", false);
    return mem;
}

nlohmann::json skill_to_json(const Skill& sk) {
    return {
        {"id", sk.id},
        {"name", sk.name},
        {"content", sk.content},
        {"trigger_phrase", sk.trigger_phrase},
        {"tags", sk.tags},
        {"evidence", sk.evidence_count},
        {"last_confirm_turn", sk.last_confirm_turn},
        {"score", sk.score},
        {"promoted", sk.promoted}};
}

Skill json_to_skill(const nlohmann::json& j) {
    Skill sk;
    sk.id = j.value("id", "");
    sk.name = j.value("name", "");
    sk.content = j.value("content", "");
    sk.trigger_phrase = j.value("trigger_phrase", "");
    for (const auto& t : j.value("tags", nlohmann::json::array())) sk.tags.push_back(t.get<std::string>());
    sk.evidence_count = j.value("evidence", 0);
    sk.last_confirm_turn = j.value("last_confirm_turn", 0);
    sk.score = j.value("score", 0.0);
    sk.promoted = j.value("promoted", false);
    return sk;
}

bool load_store(const std::string& path, std::unordered_map<std::string, Memory>& memories,
                std::unordered_map<std::string, Skill>& skills) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        nlohmann::json j;
        f >> j;
        for (const auto& m : j.value("memories", nlohmann::json::array())) {
            Memory mem = json_to_memory(m);
            memories[mem.id] = mem;
        }
        for (const auto& s : j.value("skills", nlohmann::json::array())) {
            Skill sk = json_to_skill(s);
            skills[sk.id] = sk;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool save_store(const std::string& path, const std::unordered_map<std::string, Memory>& memories,
                const std::unordered_map<std::string, Skill>& skills) {
    namespace fs = std::filesystem;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json mems = nlohmann::json::array();
    for (const auto& [id, mem] : memories) mems.push_back(memory_to_json(mem));
    j["memories"] = mems;
    nlohmann::json sks = nlohmann::json::array();
    for (const auto& [id, sk] : skills) sks.push_back(skill_to_json(sk));
    j["skills"] = sks;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

} // namespace memory_detail
} // namespace agent
