#ifndef AGENT_SKILL_CATALOG_H
#define AGENT_SKILL_CATALOG_H

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/experience.h"
#include "agent/skill_file.h"

namespace agent {

// Runtime union entry for one skill name. Precedence and overrides are already
// applied; `state` says how the skill surfaces to the agent and user.
struct SkillEntry {
    std::string name;
    SkillScope scope;
    SkillOrigin origin;
    std::string path;  // authored directory; empty for learned
    SkillMeta meta;    // authored frontmatter+body cache; learned fills body
    std::string state; // enabled | force-enabled | disabled | blocked | suppressed
};

// Persisted curation state for one skill name.
struct SkillOverride {
    std::string state; // enable | disable | block
    std::string note;
};

// A session-activated skill body, appended to the prompt copy on the next turn.
// Activation is per-agent session state (the list lives on Agent); the catalog
// itself is shared project data.
struct ActivatedSkill {
    std::string name;
    std::string body;
};

// The runtime union view of every discoverable skill (authored at all scopes +
// interop + learned) plus persisted curation. Owns lookup, body caching, and
// override filtering.
//
// THREAD SAFETY: the catalog is shared per project — every agent on the same
// workspace holds the same instance and may call it from its own worker
// thread concurrently. All public methods lock mtx_; accessors return copies
// so callers never hold a reference into mutable shared state.
class SkillCatalog {
public:
    // When `paths` is empty the default scan paths (workspace + config dir)
    // are used. `home` overrides the global overrides directory (tests).
    SkillCatalog(const Config& cfg, const SkillScanPaths& paths = {}, std::string home = "");

    // Rebuild the union from the scan roots and the learned store. Applies
    // overrides and precedence (override > project > global > interop >
    // learned). Invalidates the body cache.
    void discover(const std::vector<Skill>& learned);

    // Metadata copy for enabled/force-enabled skills; nullopt otherwise.
    std::optional<SkillEntry> lookup(const std::string& name) const;

    // Load (and cache) the SKILL.md body for `name`. Learned skills return
    // their stored content. Returns nullopt when unknown, disabled, unreadable,
    // or over the body budget (skills_body_budget_tokens).
    std::optional<std::string> read_body(const std::string& name);

    // Re-run discover() with the last-learned list (used after an authoring
    // write or /set skills refresh).
    void refresh();

    // Persist an enable/disable/block override for `name`. Returns false on
    // invalid state or write failure.
    bool apply_override(const std::string& name, const std::string& state,
                        const std::string& note = "");

    // Ordered `name: description` lines for the discovery slot, capped by
    // skills_max_discovery (scan order, project first).
    std::vector<std::string> discovery_block() const;

    // All union entries in scan order, including suppressed learned skills
    // (for `/set skills show`). Snapshot copy — see class comment.
    std::vector<SkillEntry> entries() const;

    std::map<std::string, SkillOverride> overrides() const;

    int body_budget_tokens() const { return body_budget_; }

    void set_interop_enabled(bool on);
    bool interop_enabled() const;

private:
    // Callers must hold mtx_.
    const SkillEntry* lookup_locked(const std::string& name) const;
    void discover_locked(const std::vector<Skill>& learned);
    void load_overrides_file(const std::string& path);
    bool save_overrides() const;
    void apply_override_state(const std::string& name, std::string& state) const;
    static int estimate_tokens(const std::string& text) noexcept;

    Config cfg_;
    SkillScanPaths paths_;
    bool interop_enabled_ = false;
    int max_discovery_ = 20;
    int body_budget_ = 5000;
    mutable std::mutex mtx_;
    std::vector<SkillEntry> entries_;
    std::map<std::string, SkillOverride> overrides_;
    std::map<std::string, std::string> body_cache_;
    std::vector<Skill> learned_;
    std::string project_overrides_path_;
    std::string global_overrides_path_;
};

} // namespace agent

#endif // AGENT_SKILL_CATALOG_H
