#include "agent/experience.h"
#include "agent/workspace.h"
#include "memory_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace agent {

class JsonMemoryStore : public MemoryStore {
public:
    explicit JsonMemoryStore(ExperienceConfig cfg) : cfg_(std::move(cfg)) {
        if (!cfg_.store_path.empty()) JsonMemoryStore::load(cfg_.store_path);
    }
    void set_current_turn(size_t turn) override { current_turn_ = static_cast<int>(turn); }
    void upsert(const Memory& m) override { memory_detail::upsert_memory(memories_, m, cfg_, current_turn_); }
    void upsert(const Skill& s) override { memory_detail::upsert_skill(skills_, s, cfg_, current_turn_); }
    std::vector<Memory> top_memories(size_t k, const std::string& q) const override {
        return memory_detail::select_top_memories(memories_, k, q, current_turn_);
    }
    std::vector<Skill> top_skills(size_t k, const std::string& q) const override {
        return memory_detail::select_top_skills(skills_, k, q);
    }
    std::vector<Memory> all_memories() const override {
        return memory_detail::sorted_memories(memories_, current_turn_);
    }
    std::vector<Skill> all_skills() const override {
        return memory_detail::sorted_skills(skills_, current_turn_);
    }
    bool remove(const std::string& id) override {
        if (memories_.erase(id) > 0) return true;
        return skills_.erase(id) > 0;
    }
    double score_of(const KnowledgeItem& item) const override {
        return memory_detail::compute_score(item, "", current_turn_);
    }
    bool set_promoted(const std::string& id, bool pinned) noexcept override {
        if (auto it = memories_.find(id); it != memories_.end()) {
            it->second.promoted = pinned;
            return true;
        }
        if (auto it = skills_.find(id); it != skills_.end()) {
            it->second.promoted = pinned;
            return true;
        }
        return false;
    }
    const Memory* find_memory(const std::string& name) const override {
        for (auto& [id, m] : memories_)
            if (m.name == name) return &m;
        return nullptr;
    }
    const Skill* find_skill(const std::string& name) const override {
        for (auto& [id, s] : skills_)
            if (s.name == name) return &s;
        return nullptr;
    }
    void decay_all() override { memory_detail::decay_maps(memories_, skills_, cfg_); }
    size_t store_size() const noexcept override { return memories_.size() + skills_.size(); }
    bool load(const std::string& path) override {
        return memory_detail::load_store(path, memories_, skills_);
    }
    bool save(const std::string& path) const override {
        return memory_detail::save_store(path, memories_, skills_);
    }
    int deprecate(const std::string& content) noexcept override {
        int rc = memory_detail::deprecate_one_map(content, memories_);
        if (rc >= 0) return rc;
        return memory_detail::deprecate_one_map(content, skills_);
    }
    int memory_promote_threshold() const override { return cfg_.memory_promote_threshold; }
    int skill_promote_threshold() const override { return cfg_.skill_promote_threshold; }

private:
    ExperienceConfig cfg_;
    std::unordered_map<std::string, Memory> memories_;
    std::unordered_map<std::string, Skill> skills_;
    int current_turn_ = 0;
};

std::unique_ptr<MemoryStore> make_memory_store(const ExperienceConfig& cfg) {
    return std::make_unique<JsonMemoryStore>(cfg);
}

namespace {
namespace fs = std::filesystem;
void seed_from_legacy(const std::string& store_path) {
    std::error_code ec;
    if (fs::exists(store_path, ec)) return;
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string legacy = std::string(home) + "/.amber/memories.json";
    if (legacy == store_path || !fs::exists(legacy, ec)) return;
    fs::create_directories(fs::path(store_path).parent_path(), ec);
    fs::copy_file(legacy, store_path, fs::copy_options::none, ec);
}
} // namespace

ExperienceConfig load_experience_config(const Config& cfg) {
    ExperienceConfig ec;
    if (!cfg.experience_enabled) ec.enabled = false;
    if (!cfg.experience_store_path.empty()) ec.store_path = cfg.experience_store_path;
    if (cfg.experience_max_memories > 0) ec.max_memories = static_cast<size_t>(cfg.experience_max_memories);
    if (cfg.experience_max_skills > 0) ec.max_skills = static_cast<size_t>(cfg.experience_max_skills);
    if (cfg.experience_decay_rate > 0.0) ec.decay_rate = cfg.experience_decay_rate;
    if (cfg.experience_promote_threshold > 0) ec.memory_promote_threshold = cfg.experience_promote_threshold;
    if (ec.store_path.empty()) {
        ec.store_path = Workspace::local_dir() + "/experience.json";
        seed_from_legacy(ec.store_path);
    }
    return ec;
}

} // namespace agent
