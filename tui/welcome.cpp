// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "welcome.h"
#include "widgets.h"
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

// ---- ncurses grayscale setup ----------------------------------------------
// Use the standard xterm-256color grayscale ramp (colours 232-255), which is
// already grayscale on every 256-colour terminal. We deliberately do NOT call
// init_color(): redefining palette entries emits OSC 4 escape sequences that
// terminals such as PuTTY render literally as garbage. Registering the ramp
// colours directly gives identical visuals everywhere without that side-effect.
void init_gray_pairs() {
    static bool done = false;
    if (done) return;
    done = true;

    init_pair(P_GRAY, 0, 0);
    for (int i = 0; i < 24; ++i)
        init_pair(P_GRAY + 1 + i, 232 + i, 232 + i);
}

int color_to_pair(int cidx) {
    if (cidx == 0) return P_GRAY;
    if (cidx >= 232 && cidx <= 255) return P_GRAY + 1 + (cidx - 232);
    return P_GRAY;
}

} // namespace

void render(WINDOW* win, int start_y, int width) {
    init_gray_pairs();

    int xo = std::max(0, (width - kArtCols) / 2);

    // ---- grayscale art ----------------------------------------------------
    for (int r = 0; r < kArtRows; ++r) {
        for (int c = 0; c < kArtCols; ++c) {
            int pair = color_to_pair(kArtData[r][c]);
            wattron(win, COLOR_PAIR(pair));
            mvwaddch(win, start_y + r, xo + c, ' ');
            wattroff(win, COLOR_PAIR(pair));
        }
    }

    // ---- version / help banner --------------------------------------------
    int y = start_y + kArtRows;

    const std::string ver = "v" + std::string(agent::kVersion);
    const std::string date = agent::kBuildDate;

    wattron(win, COLOR_PAIR(P_BANNER));
    mvwaddstr(win, y++, xo, "");
    mvwaddstr(win, y++, xo,
              ("        A M B E R  " + ver + "  " +
               text::glyph::middot() + "  " + date).c_str());
    mvwaddstr(win, y++, xo, "        an AI agent for the amber-CRT age");
    mvwaddstr(win, y++, xo, "");
    mvwaddstr(win, y++, xo, "        /help      show commands & getting started");
    mvwaddstr(win, y++, xo, "        /model     set provider URL, model, context");
    mvwaddstr(win, y++, xo, "        /new       open a chat window");
    mvwaddstr(win, y++, xo, "        /config    show current configuration");
    mvwaddstr(win, y++, xo, "        /close     close this window");
    mvwaddstr(win, y++, xo, "");
    mvwaddstr(win, y++, xo, "        just start typing to chat");
    mvwaddstr(win, y++, xo, "");
    std::string credit = "        transmission by ";
    credit += agent::kAuthor;
    credit += "  " + std::string(text::glyph::middot()) + "  (c) 2026 amber systems";
    mvwaddstr(win, y++, xo, credit.c_str());
    wattroff(win, COLOR_PAIR(P_BANNER));
}

} // namespace tui::welcome

