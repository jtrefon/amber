#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "tui/setting_registry.h"

namespace tui {

// The drawer's one filter predicate. What the drawer SHOWS, what Enter
// DISPATCHES and what the tab counts COUNT must all answer it identically, or a
// row becomes unselectable, or a tab counts rows nobody can see. Case-insensitive
// substring: model ids are routinely typed in the wrong case ("qwen" for
// "Qwen3.8-27B"), and a prefix-only test made "space-bunny" unreachable inside
// "stealth/space-bunny-free".
//
// Header-inline rather than a function in drawer_rows.cpp: CommandLine applies it
// too, and the command-line test links only CommandLine's own object, so a TU
// dependency here would drag the whole settings tree into that link for six lines
// of comparison.
inline bool drawer_row_matches(const std::string& row, const std::string& filter) {
    if (filter.empty())
        return true;
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(row).find(lower(filter)) != std::string::npos;
}

// Whether a row belongs to `provider`. Rows are composite "provider::id" keys;
// an empty provider means the All tab and keeps everything. A row with no
// separator belongs to no provider, so it stays visible on All rather than
// vanishing from every tab.
inline bool drawer_row_in_provider(const std::string& row, const std::string& provider) {
    if (provider.empty())
        return true;
    const auto sep = row.find("::");
    return sep != std::string::npos && row.compare(0, sep, provider) == 0;
}

// Build the drawer rows for an input line from the command tree: the
// direct children of the current namespace with their short help,
// choices and ranges, filtered to `provider` (empty = every provider).
// Pure data — no ncurses; the renderer only paints.
//
//   drawer_rows("/set provider d", settings, "")
//     → { "  deepseek  Switch the active LLM provider.",
//         "  kilocode  ..." }
std::vector<std::string> drawer_rows(const std::string& input, const SettingRegistry& settings,
                                     const std::string& provider);

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
