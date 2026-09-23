#ifndef AMBER_TUI_HELP_PAGE_H
#define AMBER_TUI_HELP_PAGE_H

#include <string>
#include <vector>

#include "tui/setting_registry.h"

// L1 (pure): assemble the `?` help page for a command path. No ncurses — the
// caller shows the rows in a modal and owns the fallbacks.
namespace tui::help_page {

// The registry key a help node resolves to: a leading '/' is stripped, then the
// token after the first space (e.g. "/set model" -> "model").
std::string key_from_node(const std::string& node);

// The top-level command a help node falls back to when no man text exists:
// a leading '/' stripped, then the token before the first space.
std::string command_from_node(const std::string& node);

// The full man page for `key` as dialog rows: the help subtitle, the man body,
// a sub-commands listing, and the choices/range trailer. Empty when the key has
// no man text — the caller then falls back to the one-line status.
std::vector<std::string> build(const SettingRegistry& settings, const std::string& key);

// The one-line status fallback for a leaf without man text: the description plus
// choices/range. Empty when the key has no help text either.
std::string fallback_line(const SettingRegistry& settings, const std::string& key);

} // namespace tui::help_page

#endif // AMBER_TUI_HELP_PAGE_H
