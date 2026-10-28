
#ifndef AGENT_SKILL_COMMANDS_H
#define AGENT_SKILL_COMMANDS_H

#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/skill_catalog.h"

namespace agent {

// Curation surface shared by the TUI (/set skills, /get skills) and any other
// host. Each function returns a human-readable status or an error message.
// `show` renders the scope table.

// Lines: "scope · origin · name · state" per entry, plus disabled/blocked
// overrides that are excluded from the union.
std::vector<std::string> skill_show_lines(const SkillCatalog& catalog);

// Author a skill (wraps author_skill). Empty string on success, else error.
std::string skill_create(SkillCatalog& catalog, const std::string& name,
                         const std::string& description, const std::string& body,
                         const std::string& scope);

// Remove the SKILL.md directory for `name` in the given scope. Empty on
// success, else error.
std::string skill_delete(SkillCatalog& catalog, const std::string& name, const std::string& scope);

// Graduate a learned skill into a global authored SKILL.md (one-way; the
// learned store is untouched). Empty on success, else error.
std::string skill_export(SkillCatalog& catalog, const std::string& name);

// Persist an enable/disable/block override for `name`. Empty on success, else
// error.
std::string skill_set_override(SkillCatalog& catalog, const std::string& name,
                               const std::string& state);

// Apply one acquisition setting: `review`, `audit`, `scope` or `index`.
// Validation lives here so the TUI and the CLI cannot disagree. Returns false
// and fills `message` with the usage line when the value is invalid.
bool apply_skill_acquisition_setting(Config& cfg, const std::string& key, const std::string& value,
                                     std::string& message);

// The acquisition settings as `/get skills settings` prints them.
std::vector<std::string> skill_acquisition_settings_lines(const Config& cfg);

// The scope as it is printed: project, global, universal, interop, system.
// One mapping, so the catalog readout and the explorer cannot disagree.
const char* skill_scope_name(SkillScope scope) noexcept;

} // namespace agent

#endif // AGENT_SKILL_COMMANDS_H
