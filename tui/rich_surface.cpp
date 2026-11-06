#include "tui/rich_surface.h"

#include "tui/glyphs.h"
#include "tui/textutil.h"

#include <algorithm>
#include <array>

namespace tui::rich {

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

Role role_for_pair(int pair) {
    if (pair < 0 || pair >= P_GRAY)
        return Role::Muted; // grayscale art is used as shading, never as a role
    return role_table()[static_cast<std::size_t>(pair)];
}

Style style_from_run(const Run& run) {
    Style style{role_for_pair(run.pair)};
    style.bold = run.bold;
    style.dim = run.dim;
    style.italic = run.italic;
    style.underline = run.under;
    return style;
}

} // namespace

Style surface_style(const Run& run) {
    return style_from_run(run);
}

void paint_surface_line(Surface& surface, int x, int y, int columns, const Line& line) {
    if (columns <= 0)
        return;
    if (line.is_hr) {
        const Style style =
            line.runs.empty() ? Style{Role::Frame} : style_from_run(line.runs.front());
        const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
        surface.draw_hline(x, y, columns, glyphs.hline, style);
        return;
    }
    int used = 0;
    for (const Run& run : line.runs) {
        const int remaining = columns - used;
        if (remaining <= 0)
            break;
        const int wanted = std::min(remaining, text_columns(run.text));
        const int placed = surface.put_text(x + used, y, run.text, style_from_run(run));
        used += placed;
        // A glyph that did not fit whole means nothing after it can either; the
        // clip is the row's right edge, not a place to resume mid-glyph.
        if (placed < wanted)
            break;
    }
}

} // namespace tui::rich
