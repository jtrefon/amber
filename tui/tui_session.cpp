// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "tui/confirm_panel.h"

#include <ctime>
#include <cstdlib>
#include <string>
#include <vector>

namespace tui {

agent::Session Tui::snapshot(Window& w) const {
    agent::Session s;
    s.id = w.session_id;
    s.model = cfg_.model;
    if (w.agent) {
        s.messages = w.agent->history();
        s.meta = w.agent->meta_;
    }
    s.derive_title();
    if (w.title != "chat" && !w.title.empty()) s.title = w.title;
    return s;
}

void Tui::autosave() {
    Window& w = win();
    if (!w.dirty || !w.agent || w.agent->history().empty()) return;
    append_line(P_STATUS, "saving session...");
    draw();
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
    if (!s.meta.empty())
        w.agent->meta_ = s.meta;
    for (const auto& m : s.messages) {
        if (m.role == "user") {
            append_line(P_USER, "> " + m.content);
        } else if (m.role == "assistant") {
            if (!m.tool_calls.is_null() && !m.tool_calls.empty()) {
                for (const auto& tc : m.tool_calls) {
                    std::string fn = tc.value("function", json::object())
                                       .value("name", "?");
                    append_line(P_STATUS,
                                std::string(text::glyph::tool()) + " " + fn);
                }
            }
            if (!m.content.empty()) {
                append_markdown(m.content);
            }
        } else if (m.role == "tool") {
            std::string preview = m.content;
            if (preview.size() > 80) { preview.resize(77); preview += "..."; }
            append_line(P_STATUS, "  \u2514 " + m.name + ": " + preview);
        }
    }
    if (!s.messages.empty()) {
        win().scroll_top = max_scroll();
    }
    append_line(P_STATUS, "loaded session " + s.id);
    draw();
}

void Tui::pick_session() {
    session_browser();
}

namespace {

// Date label for a session: "Today", "Yesterday", or "Mon DD".
std::string date_label(long long ms) {
    auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    auto now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
        return "Today";
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday - 1)
        return "Yesterday";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%b %d", &tm);
    return buf;
}

std::string fmt_time(long long ms) {
    auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    auto now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    char buf[24];
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday) {
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    } else if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday - 1) {
        std::strncpy(buf, "yesterday", sizeof(buf) - 1);
    } else {
        std::strftime(buf, sizeof(buf), "%m/%d", &tm);
    }
    return buf;
}

// Case-insensitive contains.
bool matches_filter(const std::string& title, const std::string& filter) {
    if (filter.empty()) return true;
    auto ci_find = [](const std::string& hay, const std::string& needle) {
        if (needle.size() > hay.size()) return false;
        for (size_t h = 0; h <= hay.size() - needle.size(); ++h) {
            bool ok = true;
            for (size_t n = 0; n < needle.size() && ok; ++n)
                if (std::tolower(static_cast<unsigned char>(hay[h + n]))
                    != std::tolower(static_cast<unsigned char>(needle[n])))
                    ok = false;
            if (ok) return true;
        }
        return false;
    };
    return ci_find(title, filter);
}

} // namespace

void Tui::session_browser() {
    auto all = store_.list();
    if (all.empty()) {
        append_line(P_STATUS, "no saved sessions");
        return;
    }

    int sh = height(), sw = width();
    int dw = std::min(sw - 4, 120);
    int dh = std::min(sh - 6, sh - 2);

    Dialog dlg(dh, dw, "Sessions");
    WINDOW* w = dlg.win();
    int aw = dlg.cols() - 2;       // content width
    int ah = dlg.rows() - 2;       // content height
    int list_h = ah - 2;           // rows for list (minus search bar + footer)

    std::string filter;
    int sel = 0;
    int scroll_off = 0;
    curs_set(1);                   // visible cursor for the search bar

    auto rebuild = [&]() -> std::vector<std::pair<int, int>> {
        std::vector<std::pair<int, int>> out;
        std::string last_date;
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            if (!matches_filter(all[i].title, filter)) continue;
            std::string d = date_label(all[i].updated_ms);
            if (d != last_date) {
                out.emplace_back(0, i);
                last_date = d;
            }
            out.emplace_back(1, i);
        }
        return out;
    };

    // Snap sel to the nearest session row, or -1 if none exist.
    auto snap_sel = [&](auto& disp, int& s) {
        int n = static_cast<int>(disp.size());
        if (n == 0) { s = -1; return; }
        if (s < 0) s = 0;
        if (s >= n) s = n - 1;
        // Walk forward/backward to the nearest session row.
        for (int step = 0; step < n; ++step) {
            if (s + step < n && disp[s + step].first == 1) { s += step; return; }
            if (s - step >= 0 && disp[s - step].first == 1) { s -= step; return; }
        }
        s = -1;
    };

    bool done = false;
    while (!done) {
        auto disp = rebuild();
        int nd = static_cast<int>(disp.size());
        snap_sel(disp, sel);
        if (sel < scroll_off) scroll_off = sel;
        if (sel >= scroll_off + list_h) scroll_off = sel - list_h + 1;

        // Render list — clear each row with its own attribute so highlights
        // extend full-width. The date header rows and blank rows use the
        // dialog background pair inherited from wbkgd.
        int title_w = std::max(16, aw - 34);
        for (int i = 0; i < list_h; ++i) {
            int row = 1 + i;
            int disp_idx = scroll_off + i;
            bool has_item = (disp_idx < nd);
            if (!has_item) continue;  // past end — wbkgd shows through

            auto& [typ, idx] = disp[disp_idx];
            if (typ == 0) {
                // Date header
                wattron(w, COLOR_PAIR(P_BAR_DIM) | A_BOLD);
                mvwaddstr(w, row, 1, ("  " + date_label(all[idx].updated_ms)).c_str());
                wattroff(w, COLOR_PAIR(P_BAR_DIM) | A_BOLD);
            } else {
                bool cur = (disp_idx == sel);
                if (cur) {
                    // Full-width highlight bar
                    wattron(w, COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
                    mvwaddstr(w, row, 1, std::string(aw, ' ').c_str());
                    mvwaddstr(w, row, 1, "> ");
                } else {
                    wattron(w, COLOR_PAIR(P_ASSISTANT));
                    mvwaddstr(w, row, 1, "  ");
                }
                int x = 3;
                auto& m = all[idx];
                std::string title = m.title;
                if (static_cast<int>(title.size()) > title_w) {
                    title.resize(title_w - 1);
                    title += text::glyph::ellipsis();
                }
                mvwaddnstr(w, row, x, title.c_str(), title_w);
                x += title_w + 1;
                std::string mod = m.model;
                if (mod.size() > 10) { mod.resize(9); mod += text::glyph::ellipsis(); }
                mvwaddstr(w, row, x, mod.c_str());
                x += static_cast<int>(mod.size()) + 1;
                char cnt[24];
                std::snprintf(cnt, sizeof(cnt), "%d msgs", m.message_count);
                mvwaddstr(w, row, x, cnt);
                x += static_cast<int>(std::strlen(cnt)) + 1;
                if (m.file_size > 0) {
                    char sz[16];
                    if (m.file_size > 1024 * 1024)
                        std::snprintf(sz, sizeof(sz), "%.1fMB",
                                      m.file_size / (1024.0 * 1024.0));
                    else if (m.file_size > 1024)
                        std::snprintf(sz, sizeof(sz), "%.0fKB",
                                      m.file_size / 1024.0);
                    else
                        std::snprintf(sz, sizeof(sz), "%zuB", m.file_size);
                    mvwaddstr(w, row, x, sz);
                }
                std::string ts = fmt_time(m.updated_ms);
                mvwaddstr(w, row, aw - static_cast<int>(ts.size()) + 1, ts.c_str());
                if (cur) wattroff(w, COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
                else wattroff(w, COLOR_PAIR(P_ASSISTANT));
            }
        }

        // Search bar
        std::string search_prompt = "/ " + filter;
        wattron(w, COLOR_PAIR(P_STATUS));
        mvwaddstr(w, ah - 1, 1, std::string(aw, ' ').c_str());
        mvwaddnstr(w, ah - 1, 1, search_prompt.c_str(), aw);
        wmove(w, ah - 1, 2 + static_cast<int>(filter.size()));
        wattroff(w, COLOR_PAIR(P_STATUS));

        // Footer hint
        wattron(w, A_DIM);
        mvwaddstr(w, ah, 1,
                  (text::glyph::up() + std::string(text::glyph::down()) +
                   " nav  Enter load  Del remove  / search  Esc back").c_str());
        wattroff(w, A_DIM);

        update_panels();
        doupdate();

        int c = wgetch(w);
        // Handle navigation and action keys BEFORE filter input so they
        // don't get swallowed by printable-character matching.
        if (nd > 0 && (c == KEY_DOWN || c == KEY_UP || c == KEY_NPAGE ||
                       c == KEY_PPAGE || c == '\n' || c == '\r' ||
                       c == KEY_ENTER || c == KEY_DC || c == 4)) {
            switch (c) {
                case KEY_DOWN: ++sel; break;
                case KEY_UP: --sel; break;
                case KEY_NPAGE: sel += list_h; break;
                case KEY_PPAGE: sel -= list_h; break;
                case '\n': case '\r': case KEY_ENTER:
                    if (sel >= 0)
                        { load_session(all[disp[sel].second].id); done = true; }
                    break;
                case KEY_DC: case 4: {  // Delete or Ctrl+D
                    if (sel >= 0) {
                        int del_idx = disp[sel].second;
                        std::string msg = "Delete \"" + all[del_idx].title + "\"?";
                        tui::ConfirmPanel confirm("Delete Session", msg);
                        if (confirm.run()) {
                            store_.remove(all[del_idx].id);
                            all.erase(all.begin() + del_idx);
                            sel = 0; scroll_off = 0;
                            if (all.empty()) {
                                append_line(P_STATUS, "no saved sessions");
                                done = true;
                            }
                        }
                    }
                    break;
                }
            }
        } else if (c >= 32 && c <= 126) {
            filter += static_cast<char>(c);
            sel = 0;
            scroll_off = 0;
        } else if ((c == KEY_BACKSPACE || c == 127 || c == 8) && !filter.empty()) {
            filter.pop_back();
            sel = 0;
            scroll_off = 0;
        } else if (c == 27 || c == 'q') {
            done = true;
        }
    }

    curs_set(1);
    draw();
}

void Tui::lazy_load_active() {
    auto& w = win();
    if (!w.agent || !w.agent->history().empty() || w.session_id.empty()) return;
    agent::Session s;
    if (!store_.load(w.session_id, s)) {
        w.session_id.clear();
        return;
    }
    w.agent->set_history(s.messages);
    w.lines.clear();
    for (const auto& m : s.messages) {
        if (m.role == "user")
            append_line(P_USER, "> " + m.content);
        else if (m.role == "assistant") {
            if (!m.tool_calls.is_null() && !m.tool_calls.empty()) {
                for (const auto& tc : m.tool_calls) {
                    std::string fn = tc.value("function", json::object())
                                       .value("name", "?");
                    append_line(P_STATUS, std::string(text::glyph::tool())
                                + " " + fn);
                }
            }
            if (!m.content.empty())
                append_markdown(m.content);
        } else if (m.role == "tool") {
            std::string preview = m.content;
            if (preview.size() > 80) { preview.resize(77); preview += "..."; }
            append_line(P_STATUS, "  \\u2514 " + m.name + ": " + preview);
        }
    }
    if (!s.messages.empty())
        win().scroll_top = max_scroll();
}

void Tui::switch_to(size_t idx) {
    if (idx >= windows_.size() || idx == active_) return;
    active_ = idx;
    lazy_load_active();
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

void Tui::save_workspace_now() {
    std::fprintf(stderr, "\rsaving workspace...");
    std::fflush(stderr);
    agent::WorkspaceState ws;
    for (const auto& w : windows_) {
        agent::WorkspaceState::WindowEntry we;
        we.session_id = w->session_id;
        we.title = w->title;
        we.prompt_history = w->prompt_history;
        ws.windows.push_back(we);
    }
    ws.active = active_;
    store_.save_workspace(ws);
    std::fprintf(stderr, "\rworkspace saved\n");
}
void Tui::redraw_after_modal() {
    modal_open_ = false;
    // Resolve any approvals that arrived while a modal dialog was open. Each
    // resolve shows its own (non-nested) approval dialog on the now-live loop.
    while (!pending_approvals_.empty()) {
        AgentEvent ev = std::move(pending_approvals_.front());
        pending_approvals_.pop();
        resolve_approval(ev);
    }
    touchwin(stdscr);
    draw();
    draw_input("");
    flush();
}

} // namespace tui
