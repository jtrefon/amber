// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "welcome.h"

#include <algorithm>
#include <ctime>

namespace tui {

int Tui::height() const { int y, x; getmaxyx(stdscr, y, x); (void)x; return y; }
int Tui::width() const { int y, x; getmaxyx(stdscr, y, x); (void)y; return x; }
int Tui::chat_top() const { return 0; }
int Tui::chat_height() const { return std::max(1, height() - 2); }
int Tui::lines_per_page() const { return chat_height(); }
int Tui::stream_lines() const {
    return win().stream_buf.empty()
               ? 0
               : static_cast<int>(wrap_text(win().stream_buf, width()).size());
}
int Tui::max_scroll() const {
    int m = static_cast<int>(win().lines.size()) + stream_lines() - chat_height();
    return m < 0 ? 0 : m;
}

void Tui::redraw(const std::string& input) {
    touchwin(stdscr);
    draw();
    draw_input(input);
}

size_t Tui::utf8_len(const std::string& s, size_t i) {
    return text::utf8_len(s, i);
}
std::vector<std::string> Tui::wrap_text(const std::string& text, int w) {
    return text::wrap(text, w);
}
std::string Tui::timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "[%H:%M] ", &tm);
    return buf;
}
void Tui::append_line(int color, const std::string& text) {
    append_line_ts(color, text, timestamp());
}
void Tui::append_line_ts(int color, const std::string& text,
                         const std::string& ts) {
    const std::string pad(ts.size(), ' ');
    int avail = std::max(1, width() - static_cast<int>(ts.size()));
    auto wrapped = wrap_text(text, avail);
    if (wrapped.empty()) wrapped.push_back("");
    for (size_t i = 0; i < wrapped.size(); ++i)
        win().lines.push_back({color, (i == 0 ? ts : pad) + wrapped[i]});
    if (win().lines.size() > 10000)
        win().lines.erase(win().lines.begin(), win().lines.begin() + 5000);
    win().scroll_top = max_scroll();
}
void Tui::banner(const std::string& text) {
    win().lines.push_back({P_BANNER, text});
    win().scroll_top = max_scroll();
}

int Tui::display_cols(const std::string& s) { return text::display_cols(s); }
std::wstring Tui::to_wide(const std::string& s) { return text::to_wide(s); }
std::string Tui::kfmt(long n) { return agent::bar::kfmt(n); }

int Tui::gauge_pair(double f) {
    switch (agent::bar::pressure(f)) {
        case agent::bar::Pressure::Crit: return P_GAUGE_CRIT;
        case agent::bar::Pressure::Warn: return P_GAUGE_WARN;
        default:                         return P_GAUGE_OK;
    }
}

std::string Tui::state_glyph(agent::RunState s) {
    using S = agent::RunState;
    switch (s) {
        case S::Idle:      return "\u25cb idle";
        case S::Waiting:   return "\u25cc wait";
        case S::Thinking:  return "\u25d0 think";
        case S::Streaming: return "\u25cf strm";
        case S::Tooling:   return "\u25c9 tool";
        case S::Error:     return "\u25c9 err";
    }
    return "\u25cb";
}

int Tui::state_pair(agent::RunState s) {
    using S = agent::RunState;
    switch (s) {
        case S::Thinking:  return P_GAUGE_WARN;
        case S::Streaming: return P_BAR_DIM;
        case S::Tooling:   return P_GAUGE_OK;
        case S::Error:     return P_GAUGE_CRIT;
        default:           return P_BANNER;
    }
}

std::vector<Tui::Seg> Tui::bar_segments() const {
    std::vector<Seg> segs;
    std::string wtag = "[" + std::to_string(active_ + 1) + ":" +
                       windows_[active_]->title + " " +
                       std::to_string(active_ + 1) + "/" +
                       std::to_string(windows_.size()) + "]";
    segs.push_back({wtag, P_BANNER, 3});
    segs.push_back({" [" + cfg_.model + "]", P_BANNER, 5});
    segs.push_back({" " + state_glyph(state_), state_pair(state_), 1});

    if (stats_.latency_ms >= 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  lag %.0fms", stats_.latency_ms);
        segs.push_back({b, P_BAR_DIM, 6});
    } else {
        segs.push_back({"  lag \u2014", P_BAR_DIM, 6});
    }
    if (stats_.tps > 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  %.0f t/s", stats_.tps);
        segs.push_back({b, P_BAR_DIM, 4});
    } else {
        segs.push_back({"  \u2014 t/s", P_BAR_DIM, 4});
    }
    std::string up = stats_.prompt_tokens >= 0 ? kfmt(stats_.prompt_tokens)
                                                : "\u2014";
    std::string dn = stats_.completion_tokens >= 0
                         ? kfmt(stats_.completion_tokens) : "\u2014";
    segs.push_back({"  \u2191" + up + " \u2193" + dn, P_BAR_DIM, 7});
    return segs;
}

void Tui::draw() {
    if (win().welcome_art) {
        erase();
        welcome::render(stdscr, chat_top(), width());
        draw_status_bar("welcome");
        refresh();
        return;
    }

    erase();
    int view_h = chat_height();

    std::vector<std::pair<int, std::string>> pending;
    const std::string ts = win().stream_ts.empty() ? timestamp() : win().stream_ts;
    const std::string pad(ts.size(), ' ');
    int avail = std::max(1, width() - static_cast<int>(ts.size()));
    auto push_wrapped = [&](int color, const std::string& body) {
        auto ls = wrap_text(body, avail);
        for (size_t i = 0; i < ls.size(); ++i)
            pending.push_back({color, (i == 0 ? ts : pad) + ls[i]});
    };
    if (show_reasoning_ && !win().reason_folded && !win().reason_buf.empty()) {
        pending.push_back({P_REASONING, ts + "thinking..."});
        for (auto& l : wrap_text(win().reason_buf, avail))
            pending.push_back({P_REASONING, pad + l});
    }
    if (!win().stream_buf.empty())
        push_wrapped(win().stream_color, win().stream_buf);

    int total = static_cast<int>(win().lines.size() + pending.size());
    int max_top = std::max(0, total - view_h);
    int start = std::min(win().scroll_top, max_top);

    for (int row = 0; row < view_h; ++row) {
        int idx = start + row;
        if (idx < 0 || idx >= total) continue;
        const auto& [color, text] =
            (idx < static_cast<int>(win().lines.size()))
                ? win().lines[idx]
                : pending[idx - win().lines.size()];
        bool dim = (color == P_REASONING);
        attron(COLOR_PAIR(color) | (dim ? A_DIM : 0));
        mvaddnstr(chat_top() + row, 0, text.c_str(), width());
        attroff(COLOR_PAIR(color) | (dim ? A_DIM : 0));
    }

    draw_status_bar("ln " + std::to_string(start + 1) + "/" +
                    std::to_string(total));
    refresh();
}

// ---- animated traveling gradient -----------------------------------------
// Animated travelling pulse on the status bar. Uses only attribute
// variations (A_BOLD / A_NORMAL / A_DIM) on the existing P_BAR_DIM pair
// so no extra colour pairs are needed and the look stays in-theme.
int Tui::draw_gradient(int y, int x) {
    if (state_ == agent::RunState::Idle || state_ == agent::RunState::Error) {
        anim_phase_ = 0;
        return x;
    }
    ++anim_phase_;
    constexpr int W = 10;
    for (int i = 0; i < W; ++i) {
        int center = anim_phase_ % (W * 2 - 2);
        if (center >= W) center = W * 2 - 2 - center;
        int dist = std::abs(i - center);
        // A_REVERSE swaps fg/bg so the background becomes cyan — visible
        // against the blue status bar. A space with a non-default background
        // renders as a solid colour block.
        chtype attr = COLOR_PAIR(P_BAR_DIM) | A_REVERSE;
        if (dist == 0)       attr |= A_BOLD;
        else if (dist > 2)   attr |= A_DIM;
        wattron(stdscr, attr);
        mvaddch(y, x + i, ' ');
        wattroff(stdscr, attr);
    }
    return x + W;
}

void Tui::draw_status_bar(const std::string& tail) {
    int w = width();
    int y = height() - 2;

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char clk[16];
    std::strftime(clk, sizeof(clk), "[%H:%M:%S]", &tm);
    std::string clock = clk;
    int clock_w = display_cols(clock);

    attron(COLOR_PAIR(P_BANNER));
    mvhline(y, 0, ' ', w);
    attroff(COLOR_PAIR(P_BANNER));

    std::vector<Seg> segs = bar_segments();

    bool have_ctx = (cfg_.context_size > 0);
    long ctx_used = ctx_used_ >= 0 ? ctx_used_ : 0;
    double frac = have_ctx
                      ? static_cast<double>(ctx_used) / cfg_.context_size
                      : 0.0;

    int right_w = clock_w + 1;
    int budget = w - right_w;
    if (budget < 0) budget = 0;

    int gauge_min = have_ctx ? 12 : 0;
    auto text_cols = [&]() {
        int c = 0;
        for (auto& s : segs) c += display_cols(s.text);
        return c;
    };
    while (text_cols() + gauge_min > budget && !segs.empty()) {
        int worst = -1, worst_i = -1;
        for (size_t i = 0; i < segs.size(); ++i)
            if (segs[i].drop > worst) { worst = segs[i].drop; worst_i = (int)i; }
        if (worst <= 0) break;
        segs.erase(segs.begin() + worst_i);
    }

    int x = 0;
    auto put = [&](const std::string& s, int pair) {
        if (x >= budget) return;
        std::wstring ws = to_wide(s);
        int room = budget - x;
        if (static_cast<int>(ws.size()) > room) ws.resize(room);
        attron(COLOR_PAIR(pair));
        mvaddnwstr(y, x, ws.c_str(), static_cast<int>(ws.size()));
        attroff(COLOR_PAIR(pair));
        x += static_cast<int>(ws.size());
        if (x > budget) x = budget;
    };

    for (auto& s : segs) put(s.text, s.pair);

    if (have_ctx && x < budget) {
        put("  ctx ", P_BAR_DIM);
        int cells = std::min(24, std::max(6, (budget - x) - 14));
        if (cells > 0 && x < budget) {
            put("\u2590", P_BAR_DIM);
            put(agent::bar::gauge_bar(frac, cells), gauge_pair(frac));
            put("\u258c", P_BAR_DIM);
            char b[48];
            std::snprintf(b, sizeof(b), " %d%% %s/%s",
                          static_cast<int>(frac * 100 + 0.5),
                          kfmt(ctx_used).c_str(),
                          kfmt(cfg_.context_size).c_str());
            put(b, gauge_pair(frac));
        }
    }

    if (!tail.empty() && x + display_cols(tail) + 1 < budget)
        put("  " + tail, P_BAR_DIM);

    if (clock_w < w) {
        std::wstring wc = to_wide(clock);
        attron(COLOR_PAIR(P_BAR_DIM));
        mvaddnwstr(y, w - clock_w, wc.c_str(), static_cast<int>(wc.size()));
        attroff(COLOR_PAIR(P_BAR_DIM));
    }

    // Traveling gradient indicator (visible when agent is active)
    draw_gradient(y, 12);
}

void Tui::tick_clock() {
    int total = static_cast<int>(win().lines.size());
    if (!win().stream_buf.empty()) total += stream_lines();
    int start = std::min(win().scroll_top,
                         std::max(0, total - chat_height()));
    draw_status_bar("ln " + std::to_string(start + 1) + "/" +
                    std::to_string(total));
    refresh();
}

void Tui::draw_input(const std::string& s) {
    draw_drawer(s);
    int y = height() - 1;
    move(y, 0);
    clrtoeol();
    attron(COLOR_PAIR(P_USER));
    const std::string kPrompt = "amber> ";
    std::string shown = kPrompt + s;
    mvaddnstr(y, 0, shown.c_str(), width());
    attroff(COLOR_PAIR(P_USER));
    int cx = std::min(display_cols(shown), width() - 1);
    curs_set(1);
    move(y, cx);
    refresh();
}

void Tui::update_drawer(const std::string& input) {
    bool want = drawer_wants_open(input);
    if (want && !drawer_open_) drawer_sel_ = 0;
    drawer_open_ = want;
}

std::string Tui::drawer_token(const std::string& input) {
    return palette::token(input);
}
bool Tui::drawer_has_arg(const std::string& input) {
    return palette::has_arg(input);
}
std::vector<const Command*> Tui::filter_commands(const std::string& token) {
    return palette::filter(commands(), token);
}
bool Tui::drawer_wants_open(const std::string& input) const {
    return palette::wants_open(input);
}

void Tui::draw_drawer(const std::string& input) {
    if (!drawer_open_) return;

    int bar_row = height() - 2;
    std::string token = drawer_token(input);

    std::vector<std::string> rows;
    bool arg_mode = drawer_has_arg(input);

    std::vector<const Command*> matches = filter_commands(token);
    if (arg_mode) {
        const Command* c = matches.empty() ? nullptr : matches.front();
        if (c) rows.push_back("  " + usage(*c) + "   " + c->help);
        else rows.push_back("  (no such command)");
    } else {
        for (auto* c : matches) {
            std::string u = usage(*c);
            if (u.size() < 34) u.append(34 - u.size(), ' ');
            rows.push_back("  " + u + "  " + c->help);
        }
        if (rows.empty()) rows.push_back("  (no matching command  -  Esc to cancel)");
    }

    int nsel = arg_mode ? 0 : static_cast<int>(matches.size());
    if (drawer_sel_ >= nsel) drawer_sel_ = std::max(0, nsel - 1);
    if (drawer_sel_ < 0) drawer_sel_ = 0;

    int max_rows = std::max(1, bar_row - chat_top());
    int header = 1;
    int shown = std::min<int>(rows.size(), max_rows - header);
    int top = bar_row - header - shown;

    std::string hdr = arg_mode
        ? " command usage "
        : " commands  (Tab complete  Up/Down select  Enter run  Esc cancel) ";
    move(top, 0);
    attron(COLOR_PAIR(P_STATUS) | A_BOLD);
    for (int i = 0; i < width(); ++i) addch(' ');
    mvaddnstr(top, 0, hdr.c_str(), width());
    attroff(COLOR_PAIR(P_STATUS) | A_BOLD);

    for (int i = 0; i < shown; ++i) {
        int y = top + header + i;
        move(y, 0);
        clrtoeol();
        bool sel = (!arg_mode && i == drawer_sel_);
        if (sel) attron(COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
        else attron(COLOR_PAIR(P_ASSISTANT));
        mvaddnstr(y, 0, rows[i].c_str(), width());
        if (sel) attroff(COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
        else attroff(COLOR_PAIR(P_ASSISTANT));
    }
}

std::string Tui::drawer_complete(const std::string& input) {
    return palette::complete(commands(), input, drawer_sel_);
}

int Tui::drawer_menu(const std::string& title,
                     const std::vector<std::string>& items) {
    if (items.empty()) return -1;
    int sel = 0, off = 0;
    for (;;) {
        int bar_row = height() - 2;
        int max_rows = std::max(1, bar_row - chat_top());
        int header = 1;
        int visible = std::min<int>(items.size(), max_rows - header);
        if (sel < off) off = sel;
        if (sel >= off + visible) off = sel - visible + 1;
        int top = bar_row - header - visible;

        draw();
        std::string hdr = " " + title +
            "  (Up/Down select  Enter open  Esc cancel) ";
        move(top, 0);
        attron(COLOR_PAIR(P_STATUS) | A_BOLD);
        for (int i = 0; i < width(); ++i) addch(' ');
        mvaddnstr(top, 0, hdr.c_str(), width());
        attroff(COLOR_PAIR(P_STATUS) | A_BOLD);
        for (int i = 0; i < visible; ++i) {
            int idx = off + i;
            int y = top + header + i;
            move(y, 0); clrtoeol();
            bool cur = (idx == sel);
            std::string row = (cur ? "> " : "  ") + items[idx];
            if (cur) attron(COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
            else attron(COLOR_PAIR(P_ASSISTANT));
            mvaddnstr(y, 0, row.c_str(), width());
            if (cur) attroff(COLOR_PAIR(P_BUTTON_ACT) | A_BOLD);
            else attroff(COLOR_PAIR(P_ASSISTANT));
        }
        refresh();

        int c = getch();
        int n = static_cast<int>(items.size());
        if (c == KEY_DOWN) sel = (sel + 1) % n;
        else if (c == KEY_UP) sel = (sel + n - 1) % n;
        else if (c == KEY_NPAGE) sel = std::min(n - 1, sel + visible);
        else if (c == KEY_PPAGE) sel = std::max(0, sel - visible);
        else if (c == 10 || c == 13 || c == KEY_ENTER) return sel;
        else if (c == 27 || c == 'q') return -1;
    }
}

} // namespace tui
