#ifndef AMBER_TUI_COMPLETION_CONTEXT_H
#define AMBER_TUI_COMPLETION_CONTEXT_H

#include <string>
#include <vector>

#include "tui/setting_registry.h"

// L1 (pure): derive CommandLine's completion context from the command tree.
// completions.json is the single source — no completion list is hardcoded here.
namespace tui::completion_context {

struct Context {
    std::vector<std::string> rows; // completion candidates, in display order
    std::string prefix;            // what Enter prepends to the selected row
};

// For a slash input the rows are the drawer's entry names and the prefix is
// derived from the trailing token; for any other input the rows are the
// top-level command names plus JSON-declared aliases and the prefix is empty.
Context for_input(const std::string& input, const SettingRegistry& settings);

} // namespace tui::completion_context

#endif // AMBER_TUI_COMPLETION_CONTEXT_H
