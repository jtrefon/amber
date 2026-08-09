
#include "tui/confirm_panel.h"
#include "tui/widgets.h"
#include "tui/dialog.h"
#include "tui/approval_model.h"



#include <algorithm>
#include <cstring>
#include <string>

#include <panel.h>

namespace tui {

ApprovalPanel::ApprovalPanel(const std::string& summary,
                             int timeout_sec,
                             int default_idx)
    : Panel(11, std::min(std::max(static_cast<int>(summary.size()) + 8, 50), 78),
            "Approve action?",
            {{"1-4", "pick"}, {"Enter", "confirm"}, {"Esc", "cancel"}}),
      summary_(summary),
      timeout_sec_(timeout_sec),
      sel_(std::clamp(default_idx, 0, 3)),
      remaining_(timeout_sec > 0 ? timeout_sec : 0) {}

const char* ApprovalPanel::label(int i) const {
    switch (i) {
        case 0: return "Allow once";
        case 1: return "Allow session";
        case 2: return "Always allow";
        case 3: return "Always deny";
        default: return "";
    }
}

void ApprovalPanel::draw_button(int i, int y, bool selected, bool has_timer) {
    std::string txt = std::string("[") + char('1' + i) + "] " + label(i);
    if (has_timer && remaining_ > 0)
        txt += " (" + std::to_string(remaining_) + "s)";

    int x = (content_cols() - static_cast<int>(txt.size())) / 2;
    if (x < 0) x = 0;

    if (selected) {
        wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    } else {
        wattron(content(), COLOR_PAIR(PP_ITEM));
    }
    mvwprintw(content(), y, x, "%s", txt.c_str());
    if (selected)
        wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else
        wattroff(content(), COLOR_PAIR(PP_ITEM));
}

void ApprovalPanel::draw() {
    werase(content());
    int cw = content_cols();

    // Summary line (truncated if too long)
    std::string disp = summary_;
    if (static_cast<int>(disp.size()) > cw - 4) {
        if (static_cast<int>(disp.size()) > cw - 7)
            disp.resize(static_cast<std::size_t>(cw - 7));
        disp += "...";
    }
    int sx = (cw - static_cast<int>(disp.size())) / 2;
    if (sx < 0) sx = 0;
    mvwprintw(content(), 1, sx, "%s", disp.c_str());

    // Buttons
    for (int i = 0; i < 4; ++i)
        draw_button(i, 3 + i, i == sel_, i == sel_);

    // Footer help
    const char* help = "1-4:pick  Enter:confirm  Esc:cancel";
    int hx = (cw - static_cast<int>(std::strlen(help))) / 2;
    if (hx < 0) hx = 0;
    wattron(content(), COLOR_PAIR(PP_FOOTER));
    mvwprintw(content(), 8, hx, "%s", help);
    wattroff(content(), COLOR_PAIR(PP_FOOTER));

    show();
}

agent::Approval ApprovalPanel::run() {
    curs_set(0);
    ModalScope scope;
    // This dialog switches stdscr to (near-)blocking reads; restore the main
    // loop's tick timeout on every exit path or the UI freezes after the
    // first approval.
    BlockingInputGuard input_guard;

    ApprovalModel model(timeout_sec_, 0, ApprovalModel::steady_seconds);

    draw();
    while (true) {
        model.poll();
        int rem = model.remaining_sec();
        if (rem != remaining_) {
            remaining_ = rem;
            sel_ = model.selection();
            draw();
        }
        if (model.timed_out()) {
            // Fail-safe: a timed-out dialog denies; it never auto-confirms
            // the default selection.
            sel_ = -1;
            break;
        }
        // Poll at 250 ms while a countdown runs so the timer advances
        // without keypresses; block when waiting indefinitely.
        timeout(timeout_sec_ > 0 ? 250 : -1);
        int ch = getch();
        if (ch == ERR) continue;
        if (ch == '\t' || ch == KEY_RIGHT) {
            model.select(model.selection() + 1);
            sel_ = model.selection();
            draw();
            continue;
        }
        if (ch == KEY_LEFT) {
            model.select(model.selection() - 1);
            sel_ = model.selection();
            draw();
            continue;
        }
        if (ch >= '1' && ch <= '4') {
            model.select(ch - '1');
            break;
        }
        if (ch == '\n' || ch == '\r') break;
        if (ch == 27) {  // Esc — cancel/deny
            sel_ = -1;
            break;
        }
    }

    hide();
    curs_set(1);

    return model.resolve(sel_);
}

agent::Approval approve_dialog(const std::string& summary,
                                int timeout_sec,
                                int default_idx) {
    ApprovalPanel panel(summary, timeout_sec, default_idx);
    return panel.run();
}

// Simple yes/no confirmation dialog kept for delete confirmations.
ConfirmPanel::ConfirmPanel(const std::string& title,
                           const std::string& message)
    : Panel(7, std::max(static_cast<int>(message.size()) + 6, 40),
            title,
            {{"Tab", "switch"}, {"Enter", "confirm"}, {"Esc", "cancel"}}),
      message_(message) {}

bool ConfirmPanel::run() {
    curs_set(0);
    ModalScope scope;
    // Blocking read; restore the main loop's tick timeout on exit.
    BlockingInputGuard input_guard;
    timeout(-1);
    werase(content());
    int x = (content_cols() - static_cast<int>(message_.size())) / 2;
    if (x < 0) x = 0;
    mvwprintw(content(), 1, x, "%s", message_.c_str());
    draw_buttons();
    show();

    int ch;
    bool done = false;
    bool result = false;
    while (!done && (ch = getch()) != ERR) {
        switch (ch) {
        case '\t':
        case KEY_LEFT:
        case KEY_RIGHT:
            yes_ = !yes_;
            draw_buttons();
            break;
        case '\n':
        case '\r':
            result = yes_;
            done = true;
            break;
        case 27:
            done = true;
            break;
        default:
            break;
        }
    }
    hide();
    curs_set(1);
    return result;
}

void ConfirmPanel::draw_buttons() {
    int total_w = 14;
    int x = (content_cols() - total_w) / 2;
    if (x < 0) x = 0;
    if (yes_) wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else wattron(content(), COLOR_PAIR(PP_ITEM));
    mvwprintw(content(), 3, x, " [ Yes ] ");
    if (yes_) wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else wattroff(content(), COLOR_PAIR(PP_ITEM));

    if (!yes_) wattron(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else wattron(content(), COLOR_PAIR(PP_ITEM));
    mvwprintw(content(), 3, x + 8, " [ No ] ");
    if (!yes_) wattroff(content(), A_REVERSE | COLOR_PAIR(PP_SELECT));
    else wattroff(content(), COLOR_PAIR(PP_ITEM));

    update_panels();
    doupdate();
}

} // namespace tui
