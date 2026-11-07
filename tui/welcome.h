#ifndef AMBER_TUI_WELCOME_H
#define AMBER_TUI_WELCOME_H

#include "tui/surface.h"

#include <string>
#include <vector>

// Grayscale welcome mural: a 256-colour ANSI art raster painted into the frame,
// followed by version/help text. The raster keeps its exact grayscale pairs;
// the banner uses the Title role so it follows the theme.
namespace tui {
namespace welcome {

// Paint the full welcome screen at `start_y`; `width` is the available columns
// (the art is centred when narrower).
void render(Surface& surface, int start_y, int width);

} // namespace welcome
} // namespace tui

#endif
