
#ifndef AGENT_SHELL_CLASSIFY_H
#define AGENT_SHELL_CLASSIFY_H

#include <cstdint>
#include <string>
#include <vector>

namespace agent {

// Effect of a shell command on the workspace, used by the approval gate to
// decide whether a WRITE-mode command needs a dialog. ReadOnly commands run
// free; Write commands (benign in-workspace mutations) run free in WRITE mode;
// Destructive commands and Outside paths prompt for approval.
enum class ShellEffect : std::uint8_t { ReadOnly, Write, Destructive, Outside };

// Curated destructive command patterns that always prompt for approval. Each
// pattern is a prefix of the command words ("git reset" matches
// "git reset --hard"). This is the JSON-configured dangerous-command list:
// entries are seeded into .amber/policy.json where the user can raise or lower
// their level. It is NOT the read-only allowlist — a command that matches no
// read-only rule and no destructive pattern still prompts (fail safe).
const std::vector<std::string>& destructive_command_patterns();

struct ShellClass {
    ShellEffect effect = ShellEffect::Destructive;
    std::string scope_id;  // "bash:rm", "bash:git reset", "outside:/abs/dir"
};

// Classify a shell command line against the workspace root. Purely syntactic:
// a command that cannot be proven safe is Destructive (prompt). `workspace`
// must be the absolute workspace root used for path containment.
ShellClass classify_shell(const std::string& command,
                          const std::string& workspace);

} // namespace agent

#endif // AGENT_SHELL_CLASSIFY_H
