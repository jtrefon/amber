
#ifndef AGENT_SKILL_CATALOG_H
#define AGENT_SKILL_CATALOG_H

#include <map>
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

// The runtime union view of every discoverable skill (authored at all scopes +
// interop + learned) plus persisted curation. Owns lookup, body caching, and
// override filtering. Mutation (discover/apply_override) happens on the agent
// thread; other threads read snapshots.
class SkillCatalog {
public:
    // When `paths` is empty the default scan paths (workspace + config dir)
    // are used. `home` overrides the global overrides directory (tests).
    SkillCatalog(const Config& cfg, const SkillScanPaths& paths = {},
                 std::string home = "");

    // Rebuild the union from the scan roots and the learned store. Applies
    // overrides and precedence (override > project > global > interop >
    // learned). Invalidates the body cache.
    void discover(const std::vector<Skill>& learned);

    // Metadata lookup for enabled/force-enabled skills; null otherwise.
    const SkillEntry* lookup(const std::string& name) const;

    // Load (and cache) the SKILL.md body for `name`. Learned skills return
    // their stored content. Returns nullopt when unknown, disabled, unreadable,
    // or over the body budget (skills_body_budget_tokens).
    std::optional<std::string> read_body(const std::string& name);

    // Persist an enable/disable/block override for `name`. Returns false on
    // invalid state or write failure.
    bool apply_override(const std::string& name, const std::string& state,
                        const std::string& note = "");

    // Ordered `name: description` lines for the discovery slot, capped by
    // skills_max_discovery (scan order, project first).
    std::vector<std::string> discovery_block() const;

    // All union entries in scan order, including suppressed learned skills
    // (for `/set skills show`).
    const std::vector<SkillEntry>& entries() const { return entries_; }

    const std::map<std::string, SkillOverride>& overrides() const {
        return overrides_;
    }

    int body_budget_tokens() const { return body_budget_; }

    void set_interop_enabled(bool on) { interop_enabled_ = on; }
    bool interop_enabled() const { return interop_enabled_; }

private:
    void load_overrides_file(const std::string& path);
    bool save_overrides() const;
    void apply_override_state(const std::string& name, std::string& state) const;
    static int estimate_tokens(const std::string& text) noexcept;

    Config cfg_;
    SkillScanPaths paths_;
    bool interop_enabled_ = false;
    int max_discovery_ = 20;
    int body_budget_ = 5000;
    std::vector<SkillEntry> entries_;
    std::map<std::string, SkillOverride> overrides_;
    std::map<std::string, std::string> body_cache_;
    std::string project_overrides_path_;
    std::string global_overrides_path_;
};

} // namespace agent

#endif // AGENT_SKILL_CATALOG_H
