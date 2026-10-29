
#ifndef AGENT_EXPERIENCE_H
#define AGENT_EXPERIENCE_H

#include <cstdint>
#include <memory>
#include <optional>
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
    std::string name; // human-readable label
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
    std::string action; // "upsert", "deprecate"
    int evidence = 0;
    bool promoted = false;
};

struct ExperienceConfig {
    bool enabled = true;
    std::string store_path;
    size_t max_memories = 20;
    size_t max_skills = 10;
    int memory_promote_threshold = 3;
    int skill_promote_threshold = 5;
    double decay_rate = 0.1;
    size_t max_prompt_tokens = 500;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

class MemoryStore {
public:
    virtual ~MemoryStore() = default;

    virtual void upsert(const Memory& memory) = 0;
    virtual void upsert(const Skill& skill) = 0;

    // Decrement the evidence of the item whose content matches (content is
    // the store's canonical identity key). Returns the new evidence count,
    // or 0 when the item was removed, or -1 when no item matched. The
    // store owns the id mapping — callers never touch hashes.
    virtual int deprecate(const std::string& content) {
        (void)content;
        return -1;
    }

    // Decrement by the item's stable name, which is what the extract step
    // produces. Falls back to nothing when no item carries that name.
    virtual int deprecate_named(const std::string& name) {
        (void)name;
        return -1;
    }

    // Promotion thresholds — the store owns them (single source). New
    // LLM-confirmed items start at the threshold and are promoted at once.
    virtual int memory_promote_threshold() const {
        return ExperienceConfig{}.memory_promote_threshold;
    }
    virtual int skill_promote_threshold() const {
        return ExperienceConfig{}.skill_promote_threshold;
    }

    // Set the current turn counter so the store can track
    // last_confirm_turn for freshness scoring.
    virtual void set_current_turn(size_t turn) = 0;

    virtual std::vector<Memory> top_memories(size_t k, const std::string& user_message) const = 0;
    virtual std::vector<Skill> top_skills(size_t k, const std::string& user_message) const = 0;

    // Find by name (nullopt when not found). Copies — a shared store serves
    // concurrent agent workers, so references into store state are unsafe.
    virtual std::optional<Memory> find_memory(const std::string& name) const {
        (void)name;
        return std::nullopt;
    }
    virtual std::optional<Skill> find_skill(const std::string& name) const {
        (void)name;
        return std::nullopt;
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

    // The item's score (same metric all_* sorts by; relevance to an empty
    // query, so evidence/recency-driven). Used by the /learn display so the
    // shown number matches the ordering.
    virtual double score_of(const KnowledgeItem& item) const {
        (void)item;
        return 0.0;
    }

    virtual void decay_all() = 0;

    // Monotonic counter of knowledge changes (upsert/remove/pin/decay/load).
    // The prompt block is the [system][memory] prefix, so it may be rebuilt
    // only when this moves. A fresh store starts at 0.
    virtual std::uint64_t version() const { return 0; }

    // Total number of items (memories + skills) in the store.
    virtual size_t store_size() const = 0;

    virtual bool load(const std::string& path) = 0;
    virtual bool save(const std::string& path) const = 0;
};

// Renders the learned-memory block injected after the system prompt. The
// block is a function of the store alone: selection does not depend on the
// current question, and the render is cached until the store version moves.
// One retriever belongs to one agent worker, so the cache needs no lock.
class MemoryRetriever {
public:
    explicit MemoryRetriever(const MemoryStore& store, ExperienceConfig cfg = {});
    std::string build_block() const;

private:
    std::string render_block() const;

    const MemoryStore& store_;
    ExperienceConfig cfg_;
    mutable std::string cached_;
    mutable std::uint64_t cached_version_ = 0;
    mutable bool has_cache_ = false;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<MemoryStore> make_memory_store(const ExperienceConfig& cfg);
ExperienceConfig load_experience_config(const Config& cfg);

} // namespace agent

#endif // AGENT_EXPERIENCE_H
