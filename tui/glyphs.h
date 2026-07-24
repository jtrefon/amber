// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_GLYPHS_H
#define AMBER_TUI_GLYPHS_H

#include <clocale>
#include <cstdlib>
#include <string>

// Terminal glyph selection: picks Unicode or ASCII based on UTF-8 locale.
// This is htop's two-tier approach — no ncurses ACS involved.
// Works on: Linux console (ASCII), xterm/tmux/screen/macOS (Unicode),
// and over SSH (ASCII if locale not forwarded, Unicode otherwise).
//
// Not affected by NCURSES_NO_UTF8_ACS since we don't use ACS at all.

namespace tui {
namespace glyph {

inline bool utf8_locale() {
    // Three sources: LC_ALL > LC_CTYPE > LANG
    auto is_utf8 = [](const char* s) -> bool {
        if (!s) return false;
        std::string v(s);
        for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v.find("utf-8") != std::string::npos || v.find("utf8") != std::string::npos;
    };
    return is_utf8(std::getenv("LC_ALL"))
        || is_utf8(std::getenv("LC_CTYPE"))
        || is_utf8(std::getenv("LANG"));
}

// Border glyphs — use instead of raw characters for cross-terminal rendering.
// When utf8_locale() is true: returns Unicode box-drawing codepoints as UTF-8.
// When false: returns ASCII fallback (+, -, |, ^, v).
// These are safe even on TERM=linux which doesn't support UTF-8.

struct Set {
    const char* hline;  // horizontal line
    const char* vline;  // vertical line
    const char* ul;     // upper-left corner
    const char* ur;     // upper-right corner
    const char* ll;     // lower-left corner
    const char* lr;     // lower-right corner
    const char* up;     // scroll up indicator
    const char* dn;     // scroll down indicator
    const char* ellip;  // ellipsis
    const char* bullet; // bullet
};

inline const Set& get() {
    static const Set unicode = {
        "\xe2\x94\x80",  // ─
        "\xe2\x94\x82",  // │
        "\xe2\x94\x8c",  // ┌
        "\xe2\x94\x90",  // ┐
        "\xe2\x94\x94",  // └
        "\xe2\x94\x98",  // ┘
        "\xe2\x86\x91",  // ↑
        "\xe2\x86\x93",  // ↓
        "\xe2\x80\xa6",  // …
        "\xe2\x80\xa2",  // •
    };
    static const Set ascii = {
        "-", "|", "+", "+", "+", "+", "^", "v", "...", "*"
    };
    return utf8_locale() ? unicode : ascii;
}

} // namespace glyph
} // namespace tui

#endif // AMBER_TUI_GLYPHS_H
