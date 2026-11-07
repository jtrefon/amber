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
#include <string>

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

// The shipped palettes. Classic is the default; the others trade colour for
// contrast or for attributes when the terminal cannot be trusted with colour.
enum class ThemeKind : std::uint8_t { Amber, Modern, HighContrast, Monochrome };

const char* theme_kind_name(ThemeKind kind) noexcept;
bool parse_theme_kind(const std::string& text, ThemeKind& out) noexcept;

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
    // A palette. `mode` selects the glyph set; the colour roles are the same in
    // both, because readability must not depend on box drawing.
    static Theme make(ThemeKind kind, GlyphMode mode = GlyphMode::Unicode);
    static Theme amber(GlyphMode mode = GlyphMode::Unicode) { return make(ThemeKind::Amber, mode); }

    const RoleStyle& style(Role role) const noexcept;
    // Palette builders only; screens read through style().
    void set(Role role, const RoleStyle& style) noexcept {
        const auto index = static_cast<std::size_t>(role);
        if (index < roles_.size())
            roles_[index] = style;
    }

    ThemeKind kind = ThemeKind::Amber;
    GlyphMode glyphs = GlyphMode::Unicode;

private:
    std::array<RoleStyle, static_cast<std::size_t>(Role::Count)> roles_{};
};

} // namespace tui

#endif // AMBER_TUI_THEME_H
