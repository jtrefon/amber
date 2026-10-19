
#include "agent/statusbar.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace agent::bar {

Pressure pressure(double frac) {
    if (frac > 0.85)
        return Pressure::Crit;
    if (frac >= 0.60)
        return Pressure::Warn;
    return Pressure::Ok;
}

int gauge_full_cells(double frac, int cells) {
    if (cells <= 0)
        return 0;
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    int full = static_cast<int>(frac * cells);
    if (full > cells)
        full = cells;
    return full;
}

// UTF-8 block-drawing gauge: full blocks, 1/8 partial blocks, light-shade
// track. Used on terminals that advertise a UTF-8 locale.
std::string gauge_bar(double frac, int cells) {
    static const char* eighths[] = {"",       "\u258f", "\u258e", "\u258d", "\u258c",
                                    "\u258b", "\u258a", "\u2589"}; // 1/8 .. 7/8 partial blocks
    if (cells <= 0)
        return "";
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;

    double total = frac * cells; // fractional filled cells
    int full = static_cast<int>(total);
    int rem = static_cast<int>(lround((total - full) * 8.0));
    if (rem == 8) {
        ++full;
        rem = 0;
    }
    if (full > cells) {
        full = cells;
        rem = 0;
    }

    std::string s;
    for (int i = 0; i < full; ++i)
        s += "\u2588"; // full block
    int used = full;
    if (used < cells && rem > 0) {
        s += eighths[rem];
        ++used;
    }
    for (int i = used; i < cells; ++i)
        s += "\u2591"; // light shade track
    return s;
}

// ASCII fallback gauge for terminals without a UTF-8 locale (e.g. PuTTY with a
// Latin-1 translation table, where the block glyphs render as raw bytes).
// '#' marks filled cells, space marks the track. No partial-cell precision.
std::string gauge_bar_ascii(double frac, int cells) {
    if (cells <= 0)
        return "";
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    int full = static_cast<int>(lround(frac * cells));
    if (full > cells)
        full = cells;
    std::string s;
    s.append(static_cast<size_t>(full), '#');
    s.append(static_cast<size_t>(cells - full), ' ');
    return s;
}

bool utf8() {
    // UTF-8 is the default: virtually every modern terminal uses it. The
    // opt-out exists for terminals that mis-render UTF-8 (PuTTY with a Latin-1
    // translation table, say), where the user sets AMBER_ASCII=1.
    static const bool value = [] {
        const char* off = std::getenv("AMBER_ASCII");
        if (!off)
            return true;
        const char c = off[0];
        return c != '1' && c != 'y' && c != 'Y' && c != 't' && c != 'T';
    }();
    return value;
}

const char* emdash() {
    return utf8() ? "\u2014" : "-";
}
const char* up() {
    return utf8() ? "\u2191" : "^";
}
const char* down() {
    return utf8() ? "\u2193" : "v";
}
const char* chevron_up() {
    return utf8() ? "\u25b2" : "^";
}

std::string reasoning_badge(const std::string& effort) {
    return "(" + effort + ")";
}

} // namespace agent::bar
