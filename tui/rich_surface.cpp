#include "tui/rich_surface.h"

#include "tui/glyphs.h"
#include "tui/textutil.h"

#include <algorithm>

namespace tui::rich {

namespace {

Style style_from_run(const Run& run) {
    Style style{tui::role_for_pair(run.pair)};
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
