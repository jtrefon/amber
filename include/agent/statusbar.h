// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_STATUSBAR_H
#define AGENT_STATUSBAR_H

#include <string>

// UI-agnostic helpers for the BitchX/ircII-style status bar. Kept in the core
// library (not the TUI) so the rendering math is unit-testable without ncurses.
namespace agent {
namespace bar {

// Context-window pressure buckets, used to pick a gauge color.
enum class Pressure { Ok, Warn, Crit };

// Classify a fill fraction in [0,1]: Ok <0.60, Warn 0.60-0.85, Crit >0.85.
Pressure pressure(double frac);

// Abbreviate a token count: 512 -> "512", 5000 -> "5.0k", 128000 -> "128k".
// Negative input yields "?".
std::string kfmt(long n);

// Build a smooth Unicode partial-block gauge of `cells` columns for fraction f
// in [0,1], using eighth-block glyphs (U+258F..U+2588) for sub-cell resolution
// and light-shade (U+2591) for the empty track. Returns bar glyphs only (no
// brackets or percentage). f is clamped to [0,1]; cells<=0 yields "".
std::string gauge_bar(double frac, int cells);

// Count of filled whole cells (used by tests to assert fill precision).
int gauge_full_cells(double frac, int cells);

} // namespace bar
} // namespace agent

#endif // AGENT_STATUSBAR_H
