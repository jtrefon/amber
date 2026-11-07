
#include "welcome.h"
#include "pairs.h"
#include "textutil.h"

#include "agent/version.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace tui::welcome {

namespace {

// ---- pre-rendered grayscale art data --------------------------------------
// Generated from a 256-colour ANSI raster (xterm palette). 33 rows x 120 cols.
// Values are xterm colour indices: 0 = black, 232..255 = grayscale ramp.
// clang-format off
const unsigned char kArtData[33][120] = {
#include "art_data.inc"
const int kArtRows = 33;
const int kArtCols = 120;
// clang-format on

// The xterm 232..255 grayscale ramp, registered once by the terminal adapter.
// Redefining palette entries with init_color() would emit OSC 4 sequences that
// some terminals print literally; fixed pairs avoid that.
int color_to_pair(int cidx) {
    if (cidx == 0)
        return P_GRAY;
    if (cidx >= 232 && cidx <= 255)
        return P_GRAY + 1 + (cidx - 232);
    return P_GRAY;
}

} // namespace

void render(Surface& surface, int start_y, int width) {
    const int xo = std::max(0, (width - kArtCols) / 2);

    for (int r = 0; r < kArtRows; ++r) {
        for (int c = 0; c < kArtCols; ++c) {
            Style style{Role::Surface};
            style.exact_pair = color_to_pair(kArtData[r][c]);
            surface.put(xo + c, start_y + r, Cell{" ", style});
        }
    }

    int y = start_y + kArtRows;
    const Style banner{Role::Title};
    const auto line = [&](const std::string& text) { surface.put_text(xo, y++, text, banner); };
    const std::string ver = "v" + std::string(agent::kVersion);
    const std::string date = agent::kBuildDate;

    line("");
    line("        A M B E R  " + ver + "  " + text::glyph::middot() + "  " + date);
    line("        an AI agent for the amber-CRT age");
    line("");
    line("        /help      show commands & getting started");
    line("        /set model set provider URL, model, context");
    line("        /session new open a fresh conversation");
    line("        /get config show current configuration");
    line("        /window close close this window");
    line("");
    line("        just start typing to chat");
    line("");
    std::string credit = "        transmission by ";
    credit += agent::kAuthor;
    credit += "  " + std::string(text::glyph::middot()) + "  (c) 2026 amber systems";
    line(credit);
}

} // namespace tui::welcome
