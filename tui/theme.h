#ifndef AMBER_TUI_THEME_H
#define AMBER_TUI_THEME_H

// Semantic theme roles. A role says what a cell *means* (frame, selected row,
// attention), never which colour pair it happens to use today; the terminal
// adapter resolves the role against the terminal's colour depth. The palette
// therefore travels with the cell grid instead of with the paint call, which is
// what lets a gallery, a headless test and a screen share one description.

#include <array>
#include <cstddef>
#include <cstdint>

namespace tui {

enum class Color : std::uint8_t {
    Default,
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
    DarkGray,
};

enum class GlyphMode : std::uint8_t { Unicode, Ascii };

enum class Role : std::uint8_t {
    Surface,
    SurfaceAlt,
    Frame,
    FrameFocus,
    Title,
    Text,
    Muted,
    Accent,
    Selected,
    Attention,
    Success,
    Error,
    Warning,
    Hint,
    Shadow,
    Count,
};

struct RoleStyle {
    Color fg = Color::Default;
    Color bg = Color::Default;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool reverse = false;
};

// Semantic role for a legacy widgets pair id. Rich lines, markdown and the
// shell's paint shim still name pairs; resolving them here keeps one mapping.
Role role_for_pair(int pair) noexcept;

class Theme {
public:
    // The shipped palette. `mode` selects the glyph set; the colour roles are
    // the same in both, because readability must not depend on box drawing.
    static Theme amber(GlyphMode mode = GlyphMode::Unicode);

    const RoleStyle& style(Role role) const noexcept;

    GlyphMode glyphs = GlyphMode::Unicode;

private:
    std::array<RoleStyle, static_cast<std::size_t>(Role::Count)> roles_{};
};

} // namespace tui

#endif // AMBER_TUI_THEME_H
