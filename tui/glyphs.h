// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_GLYPHS_H
#define AMBER_TUI_GLYPHS_H

#include "tui/textutil.h"

#include <string>

// Terminal glyph selection for borders and indicators.
// Uses htop's two-tier approach: Unicode when UTF-8 locale, ASCII otherwise.
// No ncurses ACS dependency — works on all terminals.
//
// The utf8 detection delegates to text::glyph::utf8() which honours the
// AMBER_ASCII=1 escape hatch for terminals with broken Unicode rendering.

namespace tui {
namespace glyph {

struct Set {
    const char* hline;  // horizontal line
    const char* vline;  // vertical line
    const char* ul;     // upper-left corner
    const char* ur;     // upper-right corner
    const char* ll;     // lower-left corner
    const char* lr;     // lower-right corner
    const char* up;     // scroll up indicator
    const char* dn;     // scroll down indicator
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
    };
    static const Set ascii = {
        "-", "|", "+", "+", "+", "+", "^", "v"
    };
    return text::glyph::utf8() ? unicode : ascii;
}

} // namespace glyph
} // namespace tui

#endif // AMBER_TUI_GLYPHS_H
