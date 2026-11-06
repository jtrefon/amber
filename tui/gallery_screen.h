#ifndef AMBER_TUI_GALLERY_SCREEN_H
#define AMBER_TUI_GALLERY_SCREEN_H

// The development screen for the render foundation: `amber --gallery`. It
// paints the gallery through the terminal adapter, switches glyph mode with
// `a`, repaints in place (so the diff path is exercised) and exits on Esc/q.
// Kept separate from Tui: the gallery is a foundation proof, not a session.

namespace tui {

int run_gallery_screen();

} // namespace tui

#endif // AMBER_TUI_GALLERY_SCREEN_H
