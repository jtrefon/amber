#include "tui/theme.h"

#include "tui/pairs.h"

#include <array>

namespace tui {

namespace {} // namespace

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
    {P_MD_HEAD, Role::Attention},
    {P_REASONING, Role::Muted},
    {P_HINT, Role::Muted},
    {P_INPUT_SHADOW, Role::Muted},
    {P_MD_CODECMT, Role::Muted},
    {P_FIELD, Role::SurfaceAlt},
    {P_FIELD_ACT, Role::Selected},
    {P_BUTTON_ACT, Role::Selected},
    {P_SHADOW, Role::Shadow},
    {P_GIT_PLUS, Role::Success},
    {P_MD_CODESTR, Role::Success},
    {P_GIT_MINUS, Role::Error},
    {P_MD_HR, Role::Frame},
    // The shell's chrome keeps the colours its pair ids promised. Collapsing
    // these onto Title/Success/Attention is what silently removed the blue bar
    // and flattened the gauge glint and the verb's fade.
    {P_BANNER, Role::Banner},
    {P_DIALOG, Role::Banner},
    {P_BRAND, Role::Brand},
    {P_GAUGE_OK, Role::GaugeOk},
    {P_GAUGE_WARN, Role::GaugeWarn},
    {P_GAUGE_CRIT, Role::GaugeCrit},
    {P_BAR_DIM, Role::GaugeTrack},
    {P_GAUGE_OK_HI, Role::GaugeBright},
    {P_GAUGE_WARN_HI, Role::GaugeBright},
    {P_GAUGE_CRIT_HI, Role::GaugeBright},
    {P_TEXT_SHINE_MID, Role::ShineMid},
    {P_TEXT_SHINE_HI, Role::ShineHi},
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

const char* theme_kind_name(ThemeKind kind) noexcept {
    switch (kind) {
    case ThemeKind::Amber:
        return "Amber";
    case ThemeKind::Modern:
        return "Modern";
    case ThemeKind::HighContrast:
        return "High contrast";
    case ThemeKind::Monochrome:
        return "Monochrome";
    }
    return "Amber";
}

bool parse_theme_kind(const std::string& text, ThemeKind& out) noexcept {
    if (text == "amber")
        out = ThemeKind::Amber;
    else if (text == "modern")
        out = ThemeKind::Modern;
    else if (text == "high-contrast" || text == "high_contrast")
        out = ThemeKind::HighContrast;
    else if (text == "monochrome")
        out = ThemeKind::Monochrome;
    else
        return false;
    return true;
}

namespace {

RoleStyle role(Color fg, Color bg, bool bold = false, bool dim = false) {
    RoleStyle style;
    style.fg = fg;
    style.bg = bg;
    style.bold = bold;
    style.dim = dim;
    return style;
}

void apply_amber(Theme& theme) {
    auto set = [&theme](Role role_id, const RoleStyle& style) { theme.set(role_id, style); };
    set(Role::Surface, role(Color::Default, Color::Default));
    set(Role::SurfaceAlt, role(Color::Default, Color::DarkGray));
    set(Role::Frame, role(Color::Cyan, Color::Default));
    set(Role::FrameFocus, role(Color::Cyan, Color::Default, true));
    set(Role::Title, role(Color::Cyan, Color::Default, true));
    set(Role::Text, role(Color::Default, Color::Default));
    set(Role::Muted, role(Color::DarkGray, Color::Default, false, true));
    set(Role::Accent, role(Color::Yellow, Color::Default));
    set(Role::Selected, role(Color::Black, Color::Yellow));
    set(Role::Attention, role(Color::Yellow, Color::Default, true));
    set(Role::Success, role(Color::Green, Color::Default));
    set(Role::Error, role(Color::Red, Color::Default, true));
    set(Role::Warning, role(Color::Yellow, Color::Default));
    set(Role::Hint, role(Color::DarkGray, Color::Default, false, true));
    set(Role::Shadow, role(Color::Black, Color::Black));
    set(Role::Banner, role(Color::White, Color::Blue));
    set(Role::Brand, role(Color::Amber, Color::Blue, true));
    set(Role::GaugeOk, role(Color::Green, Color::Blue));
    set(Role::GaugeWarn, role(Color::Yellow, Color::Blue));
    set(Role::GaugeCrit, role(Color::Red, Color::Blue));
    set(Role::GaugeTrack, role(Color::Cyan, Color::Blue));
    set(Role::GaugeBright, role(Color::White, Color::Blue));
    set(Role::ShineMid, role(Color::BrightYellow, Color::Default, true));
    set(Role::ShineHi, role(Color::White, Color::Default, true));
}

void apply_modern(Theme& theme) {
    auto set = [&theme](Role role_id, const RoleStyle& style) { theme.set(role_id, style); };
    set(Role::Surface, role(Color::Default, Color::Default));
    set(Role::SurfaceAlt, role(Color::Default, Color::DarkGray));
    set(Role::Frame, role(Color::Blue, Color::Default, true));
    set(Role::FrameFocus, role(Color::Blue, Color::Default, true));
    set(Role::Title, role(Color::White, Color::Default, true));
    set(Role::Text, role(Color::Default, Color::Default));
    set(Role::Muted, role(Color::DarkGray, Color::Default, false, true));
    set(Role::Accent, role(Color::Magenta, Color::Default, true));
    set(Role::Selected, role(Color::White, Color::Blue));
    set(Role::Attention, role(Color::Magenta, Color::Default, true));
    set(Role::Success, role(Color::Green, Color::Default, true));
    set(Role::Error, role(Color::Red, Color::Default, true));
    set(Role::Warning, role(Color::Yellow, Color::Default, true));
    set(Role::Hint, role(Color::DarkGray, Color::Default, false, true));
    set(Role::Shadow, role(Color::Black, Color::Black));
    set(Role::Banner, role(Color::White, Color::Blue));
    set(Role::Brand, role(Color::Magenta, Color::Blue, true));
    set(Role::GaugeOk, role(Color::Green, Color::Blue, true));
    set(Role::GaugeWarn, role(Color::Yellow, Color::Blue, true));
    set(Role::GaugeCrit, role(Color::Red, Color::Blue, true));
    set(Role::GaugeTrack, role(Color::Cyan, Color::Blue));
    set(Role::GaugeBright, role(Color::White, Color::Blue, true));
    set(Role::ShineMid, role(Color::Magenta, Color::Default, true));
    set(Role::ShineHi, role(Color::White, Color::Default, true));
}

void apply_high_contrast(Theme& theme) {
    auto set = [&theme](Role role_id, const RoleStyle& style) { theme.set(role_id, style); };
    set(Role::Surface, role(Color::Default, Color::Default));
    set(Role::SurfaceAlt, role(Color::Black, Color::White));
    set(Role::Frame, role(Color::White, Color::Default, true));
    set(Role::FrameFocus, role(Color::White, Color::Default, true));
    set(Role::Title, role(Color::White, Color::Default, true));
    set(Role::Text, role(Color::White, Color::Default));
    set(Role::Muted, role(Color::White, Color::Default, false, true));
    set(Role::Accent, role(Color::White, Color::Default, true));
    set(Role::Selected, role(Color::Black, Color::White));
    set(Role::Attention, role(Color::Black, Color::White, true));
    set(Role::Success, role(Color::White, Color::Default, true));
    set(Role::Error, role(Color::White, Color::Default, true));
    set(Role::Warning, role(Color::White, Color::Default, true));
    set(Role::Hint, role(Color::White, Color::Default, false, true));
    set(Role::Shadow, role(Color::Black, Color::Black));
    // The bar is a white band with black text, the strongest contrast available
    // without relying on hue. The gauge keeps its shape, not its colour.
    set(Role::Banner, role(Color::Black, Color::White, true));
    set(Role::Brand, role(Color::Black, Color::White, true));
    set(Role::GaugeOk, role(Color::Black, Color::White));
    set(Role::GaugeWarn, role(Color::Black, Color::White, true));
    set(Role::GaugeCrit, role(Color::Black, Color::White, true));
    set(Role::GaugeTrack, role(Color::Black, Color::White, false, true));
    set(Role::GaugeBright, role(Color::Black, Color::White, true));
    set(Role::ShineMid, role(Color::White, Color::Default, true));
    RoleStyle shine_hi = role(Color::White, Color::Default, true);
    shine_hi.underline = true;
    set(Role::ShineHi, shine_hi);
}

void apply_monochrome(Theme& theme) {
    auto set = [&theme](Role role_id, RoleStyle style) { theme.set(role_id, style); };
    set(Role::Surface, role(Color::Default, Color::Default));
    set(Role::SurfaceAlt, role(Color::Default, Color::Default, false, true));
    set(Role::Frame, role(Color::Default, Color::Default, true));
    set(Role::FrameFocus, role(Color::Default, Color::Default, true));
    set(Role::Title, role(Color::Default, Color::Default, true));
    set(Role::Text, role(Color::Default, Color::Default));
    set(Role::Muted, role(Color::Default, Color::Default, false, true));
    set(Role::Accent, role(Color::Default, Color::Default, true));
    RoleStyle selected = role(Color::Default, Color::Default);
    selected.reverse = true;
    set(Role::Selected, selected);
    set(Role::Attention, selected);
    set(Role::Success, role(Color::Default, Color::Default, true));
    set(Role::Error, role(Color::Default, Color::Default, true));
    set(Role::Warning, role(Color::Default, Color::Default, true));
    set(Role::Hint, role(Color::Default, Color::Default, false, true));
    set(Role::Shadow, role(Color::Default, Color::Default, false, true));
    RoleStyle banner = role(Color::Default, Color::Default);
    banner.reverse = true;
    set(Role::Banner, banner);
    RoleStyle brand = banner;
    brand.bold = true;
    set(Role::Brand, brand);
    set(Role::GaugeOk, banner);
    set(Role::GaugeWarn, banner);
    RoleStyle crit = banner;
    crit.bold = true;
    set(Role::GaugeCrit, crit);
    RoleStyle track = banner;
    track.dim = true;
    set(Role::GaugeTrack, track);
    set(Role::GaugeBright, brand);
    set(Role::ShineMid, role(Color::Default, Color::Default, true));
    RoleStyle shine_hi = role(Color::Default, Color::Default, true);
    shine_hi.underline = true;
    set(Role::ShineHi, shine_hi);
}

} // namespace

Theme Theme::make(ThemeKind kind, GlyphMode mode) {
    Theme theme;
    theme.kind = kind;
    theme.glyphs = mode;
    switch (kind) {
    case ThemeKind::Amber:
        apply_amber(theme);
        break;
    case ThemeKind::Modern:
        apply_modern(theme);
        break;
    case ThemeKind::HighContrast:
        apply_high_contrast(theme);
        break;
    case ThemeKind::Monochrome:
        apply_monochrome(theme);
        break;
    }
    return theme;
}

const RoleStyle& Theme::style(Role role) const noexcept {
    const auto index = static_cast<std::size_t>(role);
    if (index >= roles_.size())
        return roles_[static_cast<std::size_t>(Role::Surface)];
    return roles_[index];
}

} // namespace tui
