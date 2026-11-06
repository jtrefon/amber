#ifndef AMBER_TUI_RICH_SURFACE_H
#define AMBER_TUI_RICH_SURFACE_H

// Adapter from markdown's rich lines to the cell grid. `rich::Line` predates the
// Surface: it names a widgets colour pair and ncurses attribute flags. Screens
// painted into a Surface resolve those names to semantic roles here, so the
// Markdown renderer keeps one output type while the painter stays terminal-free.

#include "tui/rich.h"
#include "tui/surface.h"

namespace tui::rich {

// The role and emphasis a run means on the cell grid. A pair with no semantic
// equivalent (the grayscale art block) stays muted rather than inventing one.
Style surface_style(const Run& run);

// Paint one rich line at (x, y), clipped to `columns` display columns. A rule
// spans the full width of its own row and ignores `x`.
void paint_surface_line(Surface& surface, int x, int y, int columns, const Line& line);

} // namespace tui::rich

#endif // AMBER_TUI_RICH_SURFACE_H
