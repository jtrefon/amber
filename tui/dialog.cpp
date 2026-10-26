
#include "tui/dialog.h"
#include "tui/textutil.h"

#include <cstring>

namespace tui {

namespace {
bool* g_modal_flag = nullptr;
} // namespace

void set_modal_flag(bool* flag) {
    g_modal_flag = flag;
}

ModalScope::ModalScope() {
    if (g_modal_flag)
        *g_modal_flag = true;
}
ModalScope::~ModalScope() {
    if (g_modal_flag)
        *g_modal_flag = false;
}

void init_pairs() {
    init_pair(P_USER, COLOR_GREEN, -1);
    init_pair(P_ASSISTANT, COLOR_CYAN, -1);
    init_pair(P_STATUS, COLOR_YELLOW, -1);
    init_pair(P_DEBUG, COLOR_MAGENTA, -1);
    init_pair(P_REASONING, COLOR_WHITE, -1);
    init_pair(P_BANNER, COLOR_WHITE, COLOR_BLUE);
    init_pair(P_FIELD, COLOR_WHITE, COLOR_BLACK);
    init_pair(P_FIELD_ACT, COLOR_WHITE, COLOR_CYAN);
    init_pair(P_DIALOG, COLOR_WHITE, COLOR_BLUE);
    init_pair(P_BUTTON, COLOR_BLACK, COLOR_WHITE);
    init_pair(P_BUTTON_ACT, COLOR_WHITE, COLOR_YELLOW);
    init_pair(P_SHADOW, COLOR_BLACK, COLOR_BLACK);
    init_pair(P_GAUGE_OK, COLOR_GREEN, COLOR_BLUE);
    init_pair(P_GAUGE_WARN, COLOR_YELLOW, COLOR_BLUE);
    init_pair(P_GAUGE_CRIT, COLOR_RED, COLOR_BLUE);
    init_pair(P_BAR_DIM, COLOR_CYAN, COLOR_BLUE);
    init_pair(P_GIT_PLUS, COLOR_GREEN, -1);
    init_pair(P_GIT_MINUS, COLOR_RED, -1);
    init_pair(P_ACTIVE, COLOR_GREEN, -1);
    init_pair(P_MD_HEAD, COLOR_WHITE, -1);
    init_pair(P_MD_QUOTE, COLOR_CYAN, -1);
    init_pair(P_MD_CODE, COLOR_GREEN, -1);
    init_pair(P_MD_CODEKEY, COLOR_YELLOW, -1);
    init_pair(P_MD_CODESTR, COLOR_GREEN, -1);
    init_pair(P_MD_CODENUM, COLOR_MAGENTA, -1);
    init_pair(P_MD_CODECMT, COLOR_CYAN, -1);
    init_pair(P_MD_LINK, COLOR_WHITE, -1);
    init_pair(P_MD_TABLE, COLOR_WHITE, -1);
    init_pair(P_MD_HR, COLOR_CYAN, -1);
    // The completion hint must be visible: gray on the default background.
    init_pair(P_INPUT_SHADOW, 245, -1);
    // The working pulse. The gauge is one light shade (white on the bar, over
    // both a filled cell and the track) so the glint cannot be mistaken for a
    // pressure colour. The verb fades through the terminal's bright row (11)
    // into white, so it follows the user's theme.
    init_pair(P_GAUGE_OK_HI, COLOR_WHITE, COLOR_BLUE);
    init_pair(P_GAUGE_WARN_HI, COLOR_WHITE, COLOR_BLUE);
    init_pair(P_GAUGE_CRIT_HI, COLOR_WHITE, COLOR_BLUE);
    init_pair(P_TEXT_SHINE_MID, 11, -1);
    init_pair(P_TEXT_SHINE_HI, COLOR_WHITE, -1);
}

Dialog::Dialog(int h, int w, const std::string& title) : Panel(h, w, title) {}

} // namespace tui
