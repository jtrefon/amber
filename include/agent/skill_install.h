
#ifndef AGENT_SKILL_INSTALL_H
#define AGENT_SKILL_INSTALL_H

#include <string>
#include <vector>

namespace agent {

// Install a SKILL.md pack (tar.gz, local path or http(s) URL) into
// `dest_root` as <name>/. The archive must contain SKILL.md inside exactly
// one top-level directory whose name is a valid skill name, and the
// frontmatter must parse. Returns "" on success or a human-readable error.
std::string install_skill_pack(const std::string& source, const std::string& dest_root);

// Remove the skill directory <dest_root>/<name>. Returns "" on success or an
// error message (unknown name / unsafe name).
std::string uninstall_skill(const std::string& name, const std::string& dest_root);

// One file of an already-enumerated skill. Paths are relative to the skill
// directory and are validated again here, because the adapter that produced
// them read them from a remote tree.
struct SkillPayloadFile {
    std::string path;
    std::string contents;
};

struct SkillInstallRequest {
    std::string name;      // destination directory, kebab-case
    std::string dest_root; // <skills root>
    std::vector<SkillPayloadFile> files;
    bool replace = false;
};

struct SkillInstallResult {
    bool ok = false;
    std::string error;
    int files = 0;
    long long bytes = 0;
};

// True for a relative path with no empty, "." or ".." segment and no platform
// separator/escape. The one place both the adapter and the installer agree on
// what may be written.
bool is_safe_skill_path(const std::string& relative_path) noexcept;

// Write a fully enumerated skill as one unit: validate every path, stage into a
// temp directory beside the destination, then rename into place. Nothing is
// visible under <dest_root>/<name> until every file is written. A failed
// install leaves no temp directory and no partial skill.
SkillInstallResult install_skill_files(const SkillInstallRequest& request);

} // namespace agent

#endif // AGENT_SKILL_INSTALL_H
