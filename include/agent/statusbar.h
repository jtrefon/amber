
#ifndef AGENT_STATUSBAR_H
#define AGENT_STATUSBAR_H

#include <cstdint>
#include <string>

// UI-agnostic helpers for the BitchX/ircII-style status bar. Kept in the core
// library (not the TUI) so the rendering math is unit-testable without ncurses.
namespace agent::bar {

// Context-window pressure buckets, used to pick a gauge color.
enum class Pressure : std::uint8_t { Ok, Warn, Crit };

// Classify a fill fraction in [0,1]: Ok <0.60, Warn 0.60-0.85, Crit >0.85.
Pressure pressure(double frac);

// Build a smooth Unicode partial-block gauge of `cells` columns for fraction f
// in [0,1], using eighth-block glyphs (U+258F..U+2588) for sub-cell resolution
// and light-shade (U+2591) for the empty track. Returns bar glyphs only (no
// brackets or percentage). f is clamped to [0,1]; cells<=0 yields "".
// Use this on terminals that advertise a UTF-8 locale.
std::string gauge_bar(double frac, int cells);

// ASCII fallback gauge for non-UTF-8 terminals (e.g. PuTTY with a Latin-1
// translation table): '#' filled, space for the track, no sub-cell resolution.
// f is clamped to [0,1]; cells<=0 yields "".
std::string gauge_bar_ascii(double frac, int cells);

// Count of filled whole cells (used by tests to assert fill precision).
int gauge_full_cells(double frac, int cells);

// Width of the moving band over a region `cells` wide: a third of the region,
// never narrower than three cells. The caller may override it: the gauge runs a
// three-cell glint while the verb keeps the proportional band.
int shine_band(int cells);

// How the lit band is shaded. Gradient fades from the base through a mid shade
// to the core (the verb, where the sweep should look like light moving across
// the word). Solid keeps one shade for every lit cell -- the light/core colour
// only (the gauge, where a coloured fade reads as a second, more intense signal).
enum class ShineStyle : std::uint8_t { Gradient, Solid };

// The moving highlight swept across a gauge (or a word) while a run is live.
// `cell` is a column in a region `cells` wide, `phase` its position in [0,1)
// around the sweep. Returns 0 at rest, and for a gradient 1 for the fade and 2
// for the core, so a caller can group neighbouring cells into at most three
// shades. With ShineStyle::Solid every lit cell is 2 (the core shade); there is
// no 1.
//
// `band` is the band's width in cells (see shine_band); 0 means a third of the
// region. A band wider than the region is clamped to the region. A fixed band
// snaps its centre to whole cells, so its width stays the same as it travels;
// the proportional band keeps sub-cell travel.
//
// The band travels from just outside the left edge to just outside the right
// one, so a sweep enters and leaves cleanly instead of popping at the ends. The
// glyphs never change: only the shade does.
int shine_level(int cell, int cells, double phase, int band = 0,
                ShineStyle style = ShineStyle::Gradient);

// Terminal capability: true unless the user opted out with AMBER_ASCII.
// Deliberately not keyed on nl_langinfo(CODESET) — a failed or partial
// setlocale() would otherwise silently smear every glyph into garbage.
bool utf8();

// Decorations shared by the status line. These live in the core with the rest
// of the bar formatting so a status segment (including a plugin's) can build
// its text without including a UI header.
const char* emdash();
const char* up();
const char* down();
// A single up chevron for "scrolled away from the live edge" ("^" in ASCII).
const char* chevron_up();

// Reasoning-effort badge, e.g. "(high)".
std::string reasoning_badge(const std::string& effort);

} // namespace agent::bar

#endif // AGENT_STATUSBAR_H
