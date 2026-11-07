#ifndef AMBER_TUI_GALLERY_H
#define AMBER_TUI_GALLERY_H

// The component gallery: one surface that exercises every foundation primitive
// (frame corners and edges, role styles, the glyph set, a wide glyph, a
// selection strip, a scrollbar and every state mark). It is the visual proof of
// the foundation and the pty dev screen's content. Pure: build it and assert it
// without a terminal.

#include "tui/geometry.h"
#include "tui/surface.h"
#include "tui/theme.h"

namespace tui {

// Repaint `surface` (clearing its back buffer first) for one glyph mode.
void paint_gallery(Surface& surface, GlyphMode mode, ThemeKind kind = ThemeKind::Amber);

// Build a fresh surface and paint it. The terminal screen repaints in place
// instead, so its diff stays valid across a mode switch.
Surface render_gallery(Size size, GlyphMode mode, ThemeKind kind = ThemeKind::Amber);

} // namespace tui

#endif // AMBER_TUI_GALLERY_H
