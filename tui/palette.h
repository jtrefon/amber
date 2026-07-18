// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tui::palette {

// A slash command. The command table is the single source of truth for
// dispatch, tab-completion, the command drawer, and /help.
struct Command {
    std::string name;                 // primary name, without the leading /
    std::vector<std::string> aliases; // alternate names (no leading /)
    std::string args;                 // usage hint, e.g. "new|close <name>"
    std::string help;                 // one-line description
    std::function<void(const std::string& arg)> run;
    // Argument tab-completion: given the partially-typed argument, return the
    // list of valid completions (e.g. {"always","auto","never"}). The command
    // itself owns its option list. Return empty for free-form / no completion.
    std::function<std::vector<std::string>(const std::string&)> complete_arg;
    // Current value of the command's setting, for the no-argument help frame.
    // Return empty if the command takes no persistent setting.
    std::function<std::string()> current_value;
};

// The command-name token currently typed: text after '/' up to the first
// space. Empty string means "just a slash" (show everything).
std::string token(const std::string& input);

// Whether the input has advanced past the command name (a space is present),
// in which case the drawer stops filtering and just shows the matched usage.
bool has_arg(const std::string& input);

// Should the drawer be visible for this input? Only when '/' leads.
bool wants_open(const std::string& input);

// Commands whose name or an alias starts with `tok` (case-sensitive, as
// commands are lowercase). Empty token matches all. The returned pointers
// alias into `commands` and are valid for its lifetime.
std::vector<const Command*> filter(const std::vector<Command>& commands,
                                   const std::string& tok);

// Find a command by name or alias (both given without the leading slash).
const Command* find(const std::vector<Command>& commands,
                    const std::string& name);

// Longest common prefix of the given names.
std::string common_prefix(const std::vector<std::string>& names);

// Canonical "/name <args>" spelling for help/usage lines.
std::string usage(const Command& c);

// Tab pressed with the drawer open: complete the command name. `sel` is the
// currently highlighted match index (or <0 for none). Returns the (possibly
// rewritten) input line.
std::string complete(const std::vector<Command>& commands,
                     const std::string& input, int sel);

}  // namespace tui::palette
