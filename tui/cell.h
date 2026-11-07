#ifndef AMBER_TUI_CELL_H
#define AMBER_TUI_CELL_H

// One screen cell and its style. A cell is a whole grapheme string rather than
// a single codepoint so a combining mark can attach to the glyph it modifies,
// and `continuation` marks the second column of a double-width glyph. The
// invariant is owned by Surface: a lead cell with a wide glyph is always
// followed by exactly one continuation cell, and the pair is repaired together
// when either half is overwritten.

#include "tui/theme.h"

#include <string>

namespace tui {

struct Style {
    Role role = Role::Text;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool reverse = false;
    // A fixed widgets pair for a cell that is art, not a semantic role (the
    // welcome raster). 0 means the role resolves to a colour pair.
    int exact_pair = 0;

    bool operator==(const Style& other) const noexcept {
        return role == other.role && bold == other.bold && dim == other.dim &&
               italic == other.italic && underline == other.underline && reverse == other.reverse &&
               exact_pair == other.exact_pair;
    }
    bool operator!=(const Style& other) const noexcept { return !(*this == other); }
};

struct Cell {
    std::string glyph = " ";
    Style style;
    // True on the column after a double-width glyph. The glyph is empty there:
    // the string belongs to the lead cell so a copy never splits it.
    bool continuation = false;

    bool operator==(const Cell& other) const noexcept {
        return glyph == other.glyph && style == other.style && continuation == other.continuation;
    }
    bool operator!=(const Cell& other) const noexcept { return !(*this == other); }
};

} // namespace tui

#endif // AMBER_TUI_CELL_H
