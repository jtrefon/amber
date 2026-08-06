#pragma once

#include <string>
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
std::vector<std::string> drawer_rows(const std::string& input,
                                     const SettingRegistry& settings);

} // namespace tui
