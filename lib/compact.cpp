#include "agent/compact.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace agent::compact {

namespace {

// Two significant digits at most three characters: 9.9, 12, 130. A value that
// rounds up to 1000 overflows its unit and the caller rescales.
std::string two_significant(double v) {
    char buf[32];
    if (v < 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1f", std::round(v * 10.0) / 10.0);
        std::string out = buf;
        if (out.size() >= 2 && out.compare(out.size() - 2, 2, ".0") == 0)
            out.erase(out.size() - 2);
        return out;
    }
    if (v < 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0f", std::round(v));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.0f", std::round(v / 10.0) * 10.0);
    return buf;
}

// Scale into units until the value prints in three characters or fewer.
// `scale_at` is where the first division happens (1000 for a count, 1000 for
// bytes too), `base` is what it divides by (1000, or 1024 for binary units).
// The second loop catches a value that only overflows after rounding
// (995k -> 1000k would be four characters).
void rescale(double& v, int& unit, int max_unit, double scale_at, double base) {
    while (unit < max_unit && v >= scale_at) {
        v /= base;
        ++unit;
    }
    while (unit >= 0 && unit < max_unit && v >= 100.0 && std::round(v / 10.0) >= 100.0) {
        v /= base;
        ++unit;
    }
}

} // namespace

std::string count(long n) {
    if (n < 0)
        return "?";
    if (n < 1000)
        return std::to_string(n);
    static const char* kUnits[] = {"k", "m", "g", "t"};
    double v = static_cast<double>(n);
    int unit = -1;
    rescale(v, unit, 3, 1000.0, 1000.0);
    return two_significant(v) + kUnits[unit];
}

std::string duration_ms(long ms) {
    if (ms < 0)
        return "?";
    if (ms < 1000)
        return std::to_string(ms) + "ms";
    double v;
    const char* unit;
    if (ms < 60000) {
        v = static_cast<double>(ms) / 1000.0;
        unit = "s";
    } else if (ms < 3600000) {
        v = static_cast<double>(ms) / 60000.0;
        unit = "m";
    } else {
        v = static_cast<double>(ms) / 3600000.0;
        unit = "h";
    }
    return two_significant(v) + unit;
}

std::string percent(int p) {
    if (p < 0)
        p = 0;
    if (p > 100)
        p = 100;
    return std::to_string(p);
}

std::string bytes(long b) {
    if (b < 0)
        return "?";
    if (b < 1000)
        return std::to_string(b) + "B";
    static const char* kUnits[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int unit = -1;
    rescale(v, unit, 3, 1000.0, 1024.0);
    return two_significant(v) + kUnits[unit];
}

} // namespace agent::compact
