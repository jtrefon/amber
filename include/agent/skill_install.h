
#ifndef AGENT_SKILL_INSTALL_H
#define AGENT_SKILL_INSTALL_H

#include <string>

namespace agent {

// Install a SKILL.md pack (tar.gz, local path or http(s) URL) into
// `dest_root` as <name>/. The archive must contain SKILL.md inside exactly
// one top-level directory whose name is a valid skill name, and the
// frontmatter must parse. Returns "" on success or a human-readable error.
std::string install_skill_pack(const std::string& source,
                               const std::string& dest_root);

// Remove the skill directory <dest_root>/<name>. Returns "" on success or an
// error message (unknown name / unsafe name).
std::string uninstall_skill(const std::string& name,
                            const std::string& dest_root);

} // namespace agent

#endif // AGENT_SKILL_INSTALL_H
