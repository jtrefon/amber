// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/statusbar.h"

#include <cstdio>

namespace agent {
namespace bar {

Pressure pressure(double frac) {
    if (frac > 0.85) return Pressure::Crit;
    if (frac >= 0.60) return Pressure::Warn;
    return Pressure::Ok;
}

std::string kfmt(long n) {
    if (n < 0) return "?";
    if (n < 1000) return std::to_string(n);
    double k = n / 1000.0;
    char buf[24];
    std::snprintf(buf, sizeof(buf), k < 10 ? "%.1fk" : "%.0fk", k);
    return buf;
}

int gauge_full_cells(double frac, int cells) {
    if (cells <= 0) return 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int full = static_cast<int>(frac * cells);
    if (full > cells) full = cells;
    return full;
}

std::string gauge_bar(double frac, int cells) {
    static const char* eighths[] = {
        "", "\u258f", "\u258e", "\u258d", "\u258c",
        "\u258b", "\u258a", "\u2589"};   // 1/8 .. 7/8 partial blocks
    if (cells <= 0) return "";
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    double total = frac * cells;                 // fractional filled cells
    int full = static_cast<int>(total);
    int rem = static_cast<int>((total - full) * 8.0 + 0.5);
    if (rem == 8) { ++full; rem = 0; }
    if (full > cells) { full = cells; rem = 0; }

    std::string s;
    for (int i = 0; i < full; ++i) s += "\u2588";   // full block
    int used = full;
    if (used < cells && rem > 0) { s += eighths[rem]; ++used; }
    for (int i = used; i < cells; ++i) s += "\u2591";  // light shade track
    return s;
}

} // namespace bar
} // namespace agent
