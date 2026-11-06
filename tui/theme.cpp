#include "tui/theme.h"

namespace tui {

namespace {

RoleStyle styled(Color fg, Color bg, bool bold = false, bool dim = false) {
    RoleStyle s;
    s.fg = fg;
    s.bg = bg;
    s.bold = bold;
    s.dim = dim;
    return s;
}

} // namespace

Theme Theme::amber(GlyphMode mode) {
    Theme theme;
    theme.glyphs = mode;
    auto set = [&theme](Role role, const RoleStyle& style) {
        theme.roles_[static_cast<std::size_t>(role)] = style;
    };
    set(Role::Surface, styled(Color::Default, Color::Default));
    set(Role::SurfaceAlt, styled(Color::Default, Color::DarkGray));
    set(Role::Frame, styled(Color::Cyan, Color::Default));
    set(Role::FrameFocus, styled(Color::Cyan, Color::Default, true));
    set(Role::Title, styled(Color::Cyan, Color::Default, true));
    set(Role::Text, styled(Color::Default, Color::Default));
    set(Role::Muted, styled(Color::DarkGray, Color::Default, false, true));
    set(Role::Accent, styled(Color::Yellow, Color::Default));
    set(Role::Selected, styled(Color::Black, Color::Yellow));
    set(Role::Attention, styled(Color::Yellow, Color::Default, true));
    set(Role::Success, styled(Color::Green, Color::Default));
    set(Role::Error, styled(Color::Red, Color::Default, true));
    set(Role::Warning, styled(Color::Yellow, Color::Default));
    set(Role::Hint, styled(Color::DarkGray, Color::Default, false, true));
    set(Role::Shadow, styled(Color::Black, Color::Black));
    return theme;
}

const RoleStyle& Theme::style(Role role) const noexcept {
    const auto index = static_cast<std::size_t>(role);
    if (index >= roles_.size())
        return roles_[static_cast<std::size_t>(Role::Surface)];
    return roles_[index];
}

} // namespace tui
