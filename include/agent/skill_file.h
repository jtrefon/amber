
#ifndef AGENT_SKILL_FILE_H
#define AGENT_SKILL_FILE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace agent {

// Where an authored skill lives on disk. Precedence: project > global > interop.
enum class SkillScope : std::uint8_t { Project, Global, Interop };

// Where a skill comes from. Authored = deliberate SKILL.md packages; learned =
// procedures extracted into the experience store.
enum class SkillOrigin : std::uint8_t { Authored, Learned };

// Parsed SKILL.md frontmatter + body (common subset of the Agent Skills open
// standard). `name` and `description` are the discovery surface; `body` is
// loaded on activation.
struct SkillMeta {
    std::string name;
    std::string description;
    std::string license;
    std::string compatibility;
    json metadata = json::object();
    std::string body;
};

// A discovered authored skill. The directory name is the canonical name.
struct SkillFile {
    std::string name;
    std::string path;
    SkillScope scope;
    SkillMeta meta;
};

// Tolerant frontmatter parser. Parses the common subset of the Agent Skills
// open standard's YAML frontmatter (single-line and `>`-folded values, plus
// `metadata:` subkeys) with no YAML dependency. Unknown keys are ignored
// (forward compatibility). Returns nullopt when there is no parseable
// frontmatter block; a malformed skill is undiscoverable, never a crash.
std::optional<SkillMeta> parse_skill_meta(const std::string& contents);

// Scan one skill-directory root, one level deep. Missing root -> empty result.
// `warnings`, when non-null, receives human-readable notes for entries that
// were excluded (unreadable SKILL.md, invalid name, malformed frontmatter).
std::vector<SkillFile> scan_skill_dir(const std::string& root,
                                      SkillScope scope,
                                      std::vector<std::string>* warnings = nullptr);

// Where the scanner looks for skills.
struct SkillScanPaths {
    std::string project;   // <workspace>/.amber/skills
    std::string global;    // ~/.config/amber/skills
    std::string claude;    // <workspace>/.claude/skills (interop, opt-in)
    std::string codex;     // <workspace>/.codex/skills (interop, opt-in)
};

// Scan all roots in precedence order (project -> global -> interop). The first
// occurrence of a name wins (project shadows global, global shadows interop).
// Interop roots are scanned only when `interop_enabled`.
std::vector<SkillFile> scan_skills(const SkillScanPaths& paths,
                                   bool interop_enabled,
                                   std::vector<std::string>* warnings = nullptr);

// Resolve the scan paths from the current workspace and config dir.
SkillScanPaths default_scan_paths();

// True for the routable skill-name grammar (^[a-z0-9-]+$).
bool is_kebab_name(const std::string& name) noexcept;

} // namespace agent

#endif // AGENT_SKILL_FILE_H
