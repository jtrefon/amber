
#ifndef AMBER_TUI_WELCOME_H
#define AMBER_TUI_WELCOME_H

#include <ncurses.h>
#include <string>
#include <vector>

// Grayscale welcome mural: a 256-color ANSI art raster rendered directly onto
// the ncurses window, followed by version/help text. Kept in its own module so
// the art can evolve without touching the Tui class.
namespace tui {
namespace welcome {

// Render the full welcome screen (grayscale art + banner text). The art is
// drawn first, then the program/help lines are appended below. Call once per
// frame for the welcome window. Initialize first-draw-only ncurses color pairs
// for the grayscale shading internally. `y` is the top row, `w` is the total
// available width (the art is centered if narrower).
void render(WINDOW* win, int y, int w);

} // namespace welcome
} // namespace tui

#endif
