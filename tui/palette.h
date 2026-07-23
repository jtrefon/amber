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
    std::string name;
    std::vector<std::string> aliases;
    std::string args;
    std::string help;
    std::function<void(const std::string& arg)> run;
    std::function<std::vector<std::string>(const std::string&)> complete_arg;
    std::function<std::string()> current_value;
};

// ---------------------------------------------------------------------------
// Free helpers (stateless, used by drawer rendering and others)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Completer  —  Tab-press state machine
// ---------------------------------------------------------------------------

// Result of one Tab press.
struct TabResult {
    std::string input;                   // new input line (or unchanged)
    bool close_drawer = false;           // drawer should be hidden
    bool show_popup = false;             // show ncurses selection popup
    std::vector<std::string> popup_items; // items for the popup
};

class Completer {
public:
    // Process one Tab press.  Delegates to command-name completion (drawer)
    // or argument completion (inline / popup) depending on input shape.
    // `drawer_open` / `drawer_sel` are updated by the caller and fed back in.
    TabResult handle_tab(const std::vector<Command>& commands,
                         const std::string& input,
                         int drawer_sel);

    // Drawer items for the current input (for rendering).
    std::vector<const Command*> drawer_matches(
        const std::vector<Command>& commands,
        const std::string& input) const;

    // Called after any non-Tab key so the internal state resets.
    void reset() { consecutive_tabs_ = 0; }

private:
    int consecutive_tabs_ = 0;
    std::string last_tab_input_;  // what handle_tab returned last time

    TabResult complete_cmd_name(const std::vector<Command>& commands,
                                const std::string& input,
                                int drawer_sel);
    TabResult complete_arg(const Command& cmd,
                           const std::string& partial);
};

}  // namespace tui::palette
