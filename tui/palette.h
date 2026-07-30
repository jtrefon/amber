
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tui::palette {

// ---------------------------------------------------------------------------
// Legacy flat Command (used by draw_drawer, cmd_help, handle_slash)
// ---------------------------------------------------------------------------

struct Command {
    std::string name;
    std::vector<std::string> aliases;
    std::string args;
    std::string help;
    std::function<void(const std::string& arg)> run;
    std::function<std::vector<std::string>(const std::string&)> complete_arg = nullptr;
    std::function<std::string()> current_value = nullptr;
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
