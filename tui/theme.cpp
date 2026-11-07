#include "tui/theme.h"

#include "tui/pairs.h"

#include <array>

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

namespace {

struct PairRole {
    int pair;
    Role role;
};

constexpr PairRole kRoleMap[] = {
    {P_STATUS, Role::Accent},
    {P_ACTIVE, Role::Accent},
    {P_MD_CODE, Role::Accent},
    {P_MD_CODENUM, Role::Accent},
    {P_MD_LINK, Role::Accent},
    {P_MD_QUOTE, Role::Accent},
    {P_MD_CODEKEY, Role::Accent},
    {P_DEBUG, Role::Attention},
    {P_GAUGE_WARN, Role::Attention},
    {P_TEXT_SHINE_MID, Role::Attention},
    {P_TEXT_SHINE_HI, Role::Attention},
    {P_MD_HEAD, Role::Attention},
    {P_REASONING, Role::Muted},
    {P_BAR_DIM, Role::Muted},
    {P_HINT, Role::Muted},
    {P_INPUT_SHADOW, Role::Muted},
    {P_MD_CODECMT, Role::Muted},
    {P_BANNER, Role::Title},
    {P_BRAND, Role::Title},
    {P_FIELD, Role::SurfaceAlt},
    {P_FIELD_ACT, Role::Selected},
    {P_BUTTON_ACT, Role::Selected},
    {P_SHADOW, Role::Shadow},
    {P_GAUGE_OK, Role::Success},
    {P_GAUGE_OK_HI, Role::Success},
    {P_GIT_PLUS, Role::Success},
    {P_MD_CODESTR, Role::Success},
    {P_GAUGE_CRIT, Role::Error},
    {P_GAUGE_CRIT_HI, Role::Error},
    {P_GIT_MINUS, Role::Error},
    {P_MD_HR, Role::Frame},
};

const std::array<Role, P_GRAY>& role_table() {
    static const std::array<Role, P_GRAY> table = [] {
        std::array<Role, P_GRAY> roles{};
        roles.fill(Role::Text);
        for (const auto& mapping : kRoleMap)
            roles[static_cast<std::size_t>(mapping.pair)] = mapping.role;
        return roles;
    }();
    return table;
}

} // namespace

Role role_for_pair(int pair) noexcept {
    if (pair < 0 || pair >= P_GRAY)
        return Role::Muted; // grayscale art is shading, not a semantic role
    return role_table()[static_cast<std::size_t>(pair)];
}

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
