
#ifndef AGENT_EXPERIENCE_H
#define AGENT_EXPERIENCE_H

#include <memory>
#include <string>
#include <vector>

#include "agent/compressor.h"
#include "agent/llm.h"

namespace agent {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

struct KnowledgeItem {
    std::string id;
    std::string name;                // human-readable label
    std::string content;
    std::vector<std::string> tags;
    int evidence_count = 0;
    int last_confirm_turn = 0;
    double score = 0.0;
    bool promoted = false;
};

struct Memory : KnowledgeItem {};

struct Skill : KnowledgeItem {
    std::string trigger_phrase;
};

// One item extracted during compression, used to report results back to the UI.
struct ExtractionItem {
    std::string name;
    std::string action;  // "upsert", "deprecate"
    int evidence = 0;
    bool promoted = false;
};

struct ExperienceConfig {
    bool   enabled                  = true;
    std::string store_path;
    size_t max_memories             = 20;
    size_t max_skills               = 10;
    int    memory_promote_threshold = 3;
    int    skill_promote_threshold  = 5;
    double decay_rate               = 0.1;
    size_t max_prompt_tokens        = 500;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

class MemoryStore {
public:
    virtual ~MemoryStore() = default;

    virtual void upsert(const Memory& memory) = 0;
    virtual void upsert(const Skill& skill) = 0;

    // Set the current turn counter so the store can track
    // last_confirm_turn for freshness scoring.
    virtual void set_current_turn(size_t turn) = 0;

    virtual std::vector<Memory> top_memories(
        size_t k, const std::string& user_message) const = 0;
    virtual std::vector<Skill> top_skills(
        size_t k, const std::string& user_message) const = 0;

    // Find by name (returns null if not found).
    virtual const Memory* find_memory(const std::string& name) const {
        (void)name; return nullptr;
    }
    virtual const Skill* find_skill(const std::string& name) const {
        (void)name; return nullptr;
    }

    // Full listings for the /learn surface, sorted by score descending
    // (evidence/recency-driven; no relevance term).
    virtual std::vector<Memory> all_memories() const { return {}; }
    virtual std::vector<Skill> all_skills() const { return {}; }

    // Remove the item with `id` from either collection. Returns false when
    // the id is unknown (store unchanged).
    virtual bool remove(const std::string& id) {
        (void)id;
        return false;
    }

    // Set/clear the promoted flag on the item with `id`. Returns false when
    // the id is unknown.
    virtual bool set_promoted(const std::string& id, bool pinned) {
        (void)id;
        (void)pinned;
        return false;
    }

    virtual void decay_all() = 0;

    // Total number of items (memories + skills) in the store.
    virtual size_t store_size() const = 0;

    virtual bool load(const std::string& path) = 0;
    virtual bool save(const std::string& path) const = 0;
};

class MemoryRetriever {
public:
    explicit MemoryRetriever(const MemoryStore& store);
    std::string build_system_prompt_suffix(
        const std::string& user_message,
        size_t max_tokens = 500) const;
private:
    const MemoryStore& store_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<MemoryStore> make_memory_store(const ExperienceConfig& cfg);
ExperienceConfig load_experience_config(const Config& cfg);

} // namespace agent

#endif // AGENT_EXPERIENCE_H
