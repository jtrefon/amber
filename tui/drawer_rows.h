#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tui/setting_registry.h"

namespace tui {

// Build the drawer rows for an input line from the command tree: the
// direct children of the current namespace with their short help,
// choices and ranges. Pure data — no ncurses; the renderer only paints.
//
//   drawer_rows("/set provider d", settings)
//     → { "  deepseek  Switch the active LLM provider.",
//         "  kilocode  ..." }
std::vector<std::string> drawer_rows(const std::string& input, const SettingRegistry& settings);

std::vector<std::string> drawer_entry_names(const std::string& input,
                                            const SettingRegistry& settings);

// Clamp a drawer selection into range and return the window of rows to paint.
//
// { first_row, row_count }. The window follows the selection: a catalogue list
// is routinely taller than the terminal, and a window pinned to row 0 makes
// every row past the fold both invisible and unreachable.
//
// Pure -- no ncurses -- so the geometry is directly testable rather than
// reachable only by painting to a real terminal.
std::pair<int, int> drawer_window(int count, int max_visible, int sel);

} // namespace tui
