
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tui::palette {

// ---------------------------------------------------------------------------
// Legacy flat Command (used by draw_drawer, cmd_help, handle_slash).
// Completion and dispatch are JSON-tree driven; this struct only carries
// display metadata (name, aliases, usage, help).
// ---------------------------------------------------------------------------

struct Command {
    std::string name;
    std::string action;  // JSON tree action path (core.job.kill, ...)
    std::vector<std::string> aliases;
    std::string args;
    std::string help;
};

// Text before the first space (command name without '/').
std::string token(const std::string& input);

// Whether input has a space (argument mode).
bool has_arg(const std::string& input);

// Whether the drawer should be open (line starts with '/').
bool wants_open(const std::string& input);

// Commands whose name or alias starts with `tok`. Primary-name matches
// are listed before alias matches.
std::vector<const Command*> filter(const std::vector<Command>& commands,
                                   const std::string& tok);

// Find by name or alias.
const Command* find(const std::vector<Command>& commands,
                    const std::string& name);

// Longest common prefix of a set of strings.
std::string common_prefix(const std::vector<std::string>& names);

// "/name <args>" usage line.
std::string usage(const Command& c);

}  // namespace tui::palette
