// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/dialog.h"
#include "tui/textutil.h"

#include <cstring>

namespace tui {

namespace {
bool* g_modal_flag = nullptr;
} // namespace

void set_modal_flag(bool* flag) { g_modal_flag = flag; }

ModalScope::ModalScope() {
    if (g_modal_flag) *g_modal_flag = true;
}
ModalScope::~ModalScope() {
    if (g_modal_flag) *g_modal_flag = false;
}

void init_pairs() {
    init_pair(P_USER,       COLOR_GREEN,  -1);
    init_pair(P_ASSISTANT,  COLOR_CYAN,   -1);
    init_pair(P_STATUS,     COLOR_YELLOW, -1);
    init_pair(P_DEBUG,      COLOR_MAGENTA, -1);
    init_pair(P_REASONING,  COLOR_WHITE,  -1);
    init_pair(P_BANNER,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_FIELD,      COLOR_WHITE,  COLOR_BLACK);
    init_pair(P_FIELD_ACT,  COLOR_WHITE,  COLOR_CYAN);
    init_pair(P_DIALOG,     COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_BUTTON,     COLOR_BLACK,  COLOR_WHITE);
    init_pair(P_BUTTON_ACT, COLOR_WHITE, COLOR_YELLOW);
    init_pair(P_SHADOW,     COLOR_BLACK,  COLOR_BLACK);
    init_pair(P_GAUGE_OK,   COLOR_GREEN,   COLOR_BLUE);
    init_pair(P_GAUGE_WARN, COLOR_YELLOW,  COLOR_BLUE);
    init_pair(P_GAUGE_CRIT, COLOR_RED,     COLOR_BLUE);
    init_pair(P_BAR_DIM,    COLOR_CYAN,    COLOR_BLUE);
    init_pair(P_MD_HEAD,    COLOR_WHITE,   -1);
    init_pair(P_MD_QUOTE,   COLOR_CYAN,    -1);
    init_pair(P_MD_CODE,    COLOR_GREEN,   -1);
    init_pair(P_MD_CODEKEY, COLOR_YELLOW,  -1);
    init_pair(P_MD_CODESTR, COLOR_GREEN,   -1);
    init_pair(P_MD_CODENUM, COLOR_MAGENTA, -1);
    init_pair(P_MD_CODECMT, COLOR_CYAN,    -1);
    init_pair(P_MD_LINK,    COLOR_WHITE,   -1);
    init_pair(P_MD_TABLE,   COLOR_WHITE,   -1);
    init_pair(P_MD_HR,      COLOR_CYAN,    -1);
}

Dialog::Dialog(int h, int w, const std::string& title)
    : Panel(h, w, title) {}

} // namespace tui
