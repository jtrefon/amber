// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef COMPLETION_COMPLETER_H
#define COMPLETION_COMPLETER_H

#include "completion/command.h"

#include <string>
#include <vector>

namespace completion {

// Result of one Tab or ? press.
struct CompletionResult {
    std::string new_input;                  // what to replace the input with
    bool close_drawer = false;              // drawer should be hidden
    bool show_popup = false;                // show ncurses selection popup
    std::vector<std::string> popup_items;   // items for the popup
    std::string shadow;                     // faded suggestion after cursor
    std::vector<std::string> help_lines;    // for Cisco ?-style help display
};

// Tab-press state machine with context-sensitive completion and help.
// Stateless helpers live in filter.h; this owns only the multi-tab state.
class Completer {
public:
    Completer() = default;

    // Process one Tab press or ? trigger.
    CompletionResult complete(
        const std::vector<std::unique_ptr<Command>>& commands,
        const std::string& input,
        bool question_mark = false);

    // Drawer items for the current input (for rendering the command list).
    std::vector<const Command*> drawer_matches(
        const std::vector<std::unique_ptr<Command>>& commands,
        const std::string& input) const;

    // Call after any non-Tab / non-? key to reset multi-tab state.
    void reset() { consecutive_tabs_ = 0; last_tab_input_.clear(); }

private:
    int consecutive_tabs_ = 0;
    std::string last_tab_input_;

    // Resolve the current command context from the input buffer.
    // e.g. "/set detection l" → cmd="set detection", partial="l"
    struct Context {
        const Command* command = nullptr;
        std::string full_prefix;            // full path so far (e.g., "set detection")
        std::string partial;                // trailing partial word
        std::vector<std::string> unparsed;  // remaining path tokens
    };
    Context resolve_context(
        const std::vector<std::unique_ptr<Command>>& commands,
        const std::string& input) const;

    CompletionResult complete_top_level(
        const std::vector<std::unique_ptr<Command>>& commands,
        const std::string& input, bool question_mark,
        const Context& ctx);

    CompletionResult complete_arg_level(
        const std::vector<std::unique_ptr<Command>>& commands,
        const std::string& input, bool question_mark,
        const Context& ctx) const;

    // Build help lines for a set of commands (name  -  description).
    std::vector<std::string> help_for_commands(
        const std::vector<const Command*>& cmds) const;
};

} // namespace completion

#endif
