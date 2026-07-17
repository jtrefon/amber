// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"

#include <ctime>

namespace tui {

agent::Session Tui::snapshot(Window& w) {
    agent::Session s;
    s.id = w.session_id;
    s.model = cfg_.model;
    s.messages = w.agent ? w.agent->history() : std::vector<agent::Message>{};
    s.derive_title();
    if (w.title != "chat" && !w.title.empty()) s.title = w.title;
    return s;
}

void Tui::autosave() {
    Window& w = win();
    if (!w.dirty || !w.agent || w.agent->history().empty()) return;
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        if (w.title == "chat" && !s.title.empty()) w.title = s.title;
        w.dirty = false;
    }
}

void Tui::save_session() {
    Window& w = win();
    if (!w.agent || w.agent->history().empty()) {
        append_line(P_STATUS, "nothing to save (empty conversation)");
        return;
    }
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        w.dirty = false;
        append_line(P_STATUS, "saved session " + s.id + " (\"" + s.title + "\")");
    } else {
        append_line(P_STATUS, "save failed (could not write " + store_.dir() + ")");
    }
}

void Tui::load_session(const std::string& id) {
    agent::Session s;
    if (!store_.load(id, s)) {
        append_line(P_STATUS, "load failed: no session " + id);
        return;
    }
    Window& w = new_window(s.title.empty() ? "chat" : s.title);
    w.session_id = s.id;
    w.agent->set_history(s.messages);
    for (const auto& m : s.messages) {
        if (m.role == "user") append_line(P_USER, "> " + m.content);
        else if (m.role == "assistant" && !m.content.empty())
            append_line(P_ASSISTANT, m.content);
    }
    append_line(P_STATUS, "loaded session " + s.id);
    draw();
}

void Tui::pick_session() {
    session_browser();
}

static std::string fmt_time(long long ms) {
    auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[24];
    // Today: show HH:MM; yesterday: "yesterday"; older: "MM/DD"
    std::time_t now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    bool same_day = (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday);
    bool yesterday = (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday - 1);
    if (same_day) {
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    } else if (yesterday) {
        std::strncpy(buf, "yesterday", sizeof(buf));
    } else {
        std::strftime(buf, sizeof(buf), "%m/%d", &tm);
    }
    return buf;
}

void Tui::session_browser() {
    auto metas = store_.list();
    if (metas.empty()) {
        append_line(P_STATUS, "no saved sessions");
        return;
    }

    int sh = height(), sw = width();
    int dw = std::min(sw - 4, 66);
    int dh = std::min(sh - 4, std::max(static_cast<int>(metas.size()) + 4, 8));

    Dialog dlg(dh, dw, "Sessions");
    WINDOW* w = dlg.win();
    int aw = dlg.cols() - 2;        // content width (inside borders)
    int ah = dlg.rows() - 2;        // content height
    int list_h = ah - 1;            // rows for session list (minus footer)

    int sel = 0, off = 0;
    curs_set(0);

    bool done = false;
    while (!done) {
        int n = static_cast<int>(metas.size());
        if (sel < 0) sel = 0;
        if (sel >= n) sel = n - 1;
        if (sel < off) off = sel;
        if (sel >= off + list_h) off = sel - list_h + 1;

        // Clear content area
        for (int r = 0; r < ah; ++r) {
            mvwaddstr(w, 1 + r, 1, std::string(aw, ' ').c_str());
        }

        // Render list items
        int title_w = std::max(18, aw - 34);  // room for model + count + time
        for (int i = 0; i < list_h && off + i < n; ++i) {
            int idx = off + i;
            auto& m = metas[idx];
            bool cur = (idx == sel);
            int row = 1 + i;
            int x = 1;

            if (cur) {
                wattron(w, COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
                mvwaddstr(w, row, x, "> ");
            } else {
                wattron(w, COLOR_PAIR(P_ASSISTANT));
                mvwaddstr(w, row, x, "  ");
            }
            x += 2;

            // Title (truncated)
            std::string title = m.title;
            if (static_cast<int>(title.size()) > title_w)
                title = title.substr(0, title_w - 1) + "\u2026";
            mvwaddnstr(w, row, x, title.c_str(), title_w);
            x += title_w + 1;

            // Model (short)
            std::string mod = m.model;
            if (mod.size() > 10) mod = mod.substr(0, 9) + "\u2026";
            mvwaddstr(w, row, x, mod.c_str());
            x += static_cast<int>(mod.size()) + 1;

            // Message count
            char cnt[16];
            std::snprintf(cnt, sizeof(cnt), "%d msgs", m.message_count);
            mvwaddstr(w, row, x, cnt);

            // Time (right-aligned)
            std::string ts = fmt_time(m.updated_ms);
            mvwaddstr(w, row, aw - static_cast<int>(ts.size()) + 1, ts.c_str());

            if (cur) wattroff(w, COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
            else wattroff(w, COLOR_PAIR(P_ASSISTANT));
        }

        // Footer hint
        wattron(w, A_DIM);
        mvwaddstr(w, ah, 1, "\u2191\u2193 nav  Enter load  Del remove  Esc back");
        wattroff(w, A_DIM);

        update_panels();
        doupdate();

        int c = wgetch(w);
        switch (c) {
            case KEY_DOWN: case '\t': ++sel; break;
            case KEY_UP: case KEY_BTAB: --sel; break;
            case KEY_NPAGE: sel += list_h; break;
            case KEY_PPAGE: sel -= list_h; break;
            case '\n': case '\r': case KEY_ENTER:
                load_session(metas[sel].id);
                done = true;
                break;
            case KEY_DC: case 'd': {
                // Confirm delete
                std::string prompt = "Delete \"" + metas[sel].title + "\"?";
                int ch = menu_select(prompt, {"Cancel", "Delete"});
                if (ch == 1) {
                    store_.remove(metas[sel].id);
                    metas.erase(metas.begin() + sel);
                    if (metas.empty()) {
                        append_line(P_STATUS, "no saved sessions");
                        done = true;
                    }
                }
                break;
            }
            case 27: case 'q': done = true; break;
        }
    }

    curs_set(1);
    draw();
}

void Tui::switch_to(size_t idx) {
    if (idx >= windows_.size() || idx == active_) return;
    active_ = idx;
    draw();
}

void Tui::close_window() {
    if (windows_.size() <= 1) {
        append_line(P_STATUS, "cannot close the last window");
        return;
    }
    autosave();
    windows_.erase(windows_.begin() + active_);
    if (active_ >= windows_.size()) active_ = windows_.size() - 1;
    draw();
}

void Tui::request_quit() { quit_ = true; }
void Tui::redraw_after_modal() { touchwin(stdscr); draw(); draw_input(""); }

} // namespace tui
