// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>

#include "widgets.h"
#include "textutil.h"
#include "window.h"
#include "palette.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using tui::Dialog;
using tui::FieldSpec;
using tui::Pair;
using tui::form_edit;
using tui::info_dialog;
using tui::menu_select;
using tui::P_ASSISTANT;
using tui::P_BUTTON_ACT;
using tui::P_BANNER;
using tui::P_STATUS;
using tui::P_REASONING;
using tui::P_USER;
using tui::P_GAUGE_OK;
using tui::P_GAUGE_WARN;
using tui::P_GAUGE_CRIT;
using tui::P_BAR_DIM;

using tui::Window;
using Command = tui::palette::Command;

class Tui {
public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg)
        : cfg_(std::move(cfg)), reg_(reg), store_() {
        // Honour the user's locale so ncursesw can encode/decode multibyte
        // UTF-8 glyphs (box drawing, block gauges, dots). Must precede initscr.
        std::setlocale(LC_ALL, "");
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        set_escdelay(25);
        curs_set(0);
        start_color();
        use_default_colors();
        tui::init_pairs();

        new_window("chat");   // always have one conversation to talk to
    }

    ~Tui() {
        // Persist every window with unsaved work before tearing down curses.
        for (size_t i = 0; i < windows_.size(); ++i) {
            Window& w = *windows_[i];
            if (!w.dirty || !w.agent || w.agent->history().empty()) continue;
            agent::Session s = snapshot(w);
            if (store_.save(s)) w.session_id = s.id;
        }
        endwin();
    }

    // Create a fresh window with its own stateful Agent and make it active.
    Window& new_window(const std::string& title) {
        auto w = std::make_unique<Window>();
        w->title = title;
        w->agent = std::make_unique<agent::Agent>(cfg_, reg_);
        windows_.push_back(std::move(w));
        active_ = windows_.size() - 1;
        return *windows_.back();
    }

    void run() {
        draw();
        banner("amber - F1 help  /help commands  Ctrl-N new win  "
               "Alt+1..9 switch  Enter send  Ctrl-C quit");
        draw_input("");
        detect_server(false);   // auto-fill model/context from the server

        // Wake once a second even without input so the status-bar clock ticks.
        // This is a blocking poll with a 1s timeout, not a busy loop: idle CPU
        // is one wakeup per second (negligible).
        timeout(1000);

        std::string input;
        while (!quit_) {
            int ch = getch();
            if (ch == ERR) { tick_clock(); draw_input(input); continue; }
            if (ch == KEY_F(1)) { help_screen(); redraw(input); continue; }
            if (ch == KEY_F(2)) { config_screen(); redraw(input); continue; }
            if (ch == KEY_F(10)) { settings_screen(); redraw(input); continue; }
            if (ch == KEY_F(3)) {
                cfg_.show_reasoning = !cfg_.show_reasoning;
                append_line(P_STATUS, std::string("thinking display: ") +
                                          (cfg_.show_reasoning ? "on" : "off"));
                draw(); draw_input(input); continue;
            }
            if (ch == 14) {   // Ctrl-N: new window
                new_window("chat");
                draw(); draw_input(input); continue;
            }
            if (ch == 23) {   // Ctrl-W: close window
                close_window();
                draw(); draw_input(input); continue;
            }
            if (ch == 27) {   // ESC: close the drawer, else Alt+<digit> window jump
                if (drawer_open_) {
                    drawer_open_ = false;
                    draw(); draw_input(input);
                    continue;
                }
                int n = getch();
                if (n >= '1' && n <= '9') {
                    switch_to(static_cast<size_t>(n - '1'));
                    draw_input(input);
                }
                continue;
            }
            if (ch == '\t' && drawer_open_) {   // Tab: complete the command name
                input = drawer_complete(input);
                drawer_sel_ = 0;
                draw(); draw_input(input);
                continue;
            }
            if (drawer_open_ && (ch == KEY_UP || ch == KEY_DOWN)) {
                int n = static_cast<int>(filter_commands(drawer_token(input)).size());
                if (n > 0) {
                    if (ch == KEY_DOWN) drawer_sel_ = (drawer_sel_ + 1) % n;
                    else drawer_sel_ = (drawer_sel_ + n - 1) % n;
                }
                draw_input(input);
                continue;
            }
            if (ch == KEY_NPAGE) {
                win().scroll_top = std::min(max_scroll(),
                                       win().scroll_top + lines_per_page());
                draw(); draw_input(input); continue;
            }
            if (ch == KEY_PPAGE) {
                win().scroll_top = std::max(0, win().scroll_top - lines_per_page());
                draw(); draw_input(input); continue;
            }
            if (ch == 7 || ch == 10 || ch == 13 || ch == KEY_ENTER) {
                // Drawer open on a bare command name: Enter picks the highlighted
                // command. If it takes arguments, fill the line and keep editing;
                // otherwise run it immediately.
                if (drawer_open_ && !drawer_has_arg(input)) {
                    auto matches = filter_commands(drawer_token(input));
                    if (!matches.empty() &&
                        drawer_sel_ < static_cast<int>(matches.size())) {
                        // Enter runs the highlighted command immediately (with an
                        // empty argument). To pass an argument, type a space after
                        // the name first (or Tab to complete, then type it).
                        const Command* c = matches[drawer_sel_];
                        drawer_open_ = false;
                        input.clear();
                        handle_slash("/" + c->name);
                        draw(); draw_input("");
                        continue;
                    }
                }
                if (input.empty()) continue;
                std::string prompt = input;
                drawer_open_ = false;
                if (handle_slash(prompt)) {
                    input.clear();
                    draw(); draw_input("");
                    continue;
                }
                append_line(P_USER, "> " + input);
                input.clear();
                draw_input("");
                send(prompt);
                draw_input("");
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!input.empty()) input.pop_back();
                update_drawer(input);
                draw(); draw_input(input);
            } else if (ch >= 32 && ch <= 126) {
                input += static_cast<char>(ch);
                update_drawer(input);
                draw(); draw_input(input);
            }
        }
    }

private:
    int height() const { int y, x; getmaxyx(stdscr, y, x); (void)x; return y; }
    int width() const { int y, x; getmaxyx(stdscr, y, x); (void)y; return x; }
    // Layout: row 0 window-tab line, rows 1..H-3 chat, row H-2 status bar,
    // row H-1 input.
    int chat_top() const { return 1; }
    int chat_height() const { return std::max(1, height() - 3); }
    int lines_per_page() const { return chat_height(); }
    int stream_lines() const {
        return win().stream_buf.empty()
                   ? 0
                   : static_cast<int>(wrap_text(win().stream_buf, width()).size());
    }
    int max_scroll() const {
        int m = static_cast<int>(win().lines.size()) + stream_lines() - chat_height();
        return m < 0 ? 0 : m;
    }

    void redraw(const std::string& input) {
        touchwin(stdscr);
        draw();
        draw_input(input);
    }

    // Wrap `text` into display lines: honour embedded newlines, then word-wrap
    // each paragraph to `w` columns (falling back to hard splits for words
    // longer than the width). Tabs are expanded to spaces.
    // Byte length of the UTF-8 sequence starting at s[i] (1 on invalid lead).
    static size_t utf8_len(const std::string& s, size_t i) {
        return tui::text::utf8_len(s, i);
    }

    static std::vector<std::string> wrap_text(const std::string& text, int w) {
        return tui::text::wrap(text, w);
    }

    // IRC (BitchX/ircII) style local timestamp, e.g. "[23:04] ".
    static std::string timestamp() {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "[%H:%M] ", &tm);
        return buf;
    }

    // Commit a chat message prefixed with an IRC-style timestamp. The stamp is
    // shown once on the first line; wrapped continuation lines are indented to
    // align under the text, matching ircII/BitchX behaviour.
    void append_line(int color, const std::string& text) {
        append_line_ts(color, text, timestamp());
    }

    // As append_line, but with a caller-supplied timestamp so a streamed reply
    // commits under the same stamp it was shown with while streaming.
    void append_line_ts(int color, const std::string& text,
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

    void banner(const std::string& text) {
        win().lines.push_back({P_BANNER, text});
        win().scroll_top = max_scroll();
    }

    // Top line: IRC-style window tabs, e.g. "[1:chat][2:bugfix]" with the active
    // window highlighted. Rendered with the wide-char API for correct columns.
    void draw_tabs() {
        move(0, 0);
        attron(COLOR_PAIR(P_STATUS));
        for (int i = 0; i < width(); ++i) addch(' ');
        int col = 0;
        for (size_t i = 0; i < windows_.size() && col < width(); ++i) {
            std::string label =
                "[" + std::to_string(i + 1) + ":" + windows_[i]->title + "]";
            bool act = (i == active_);
            if (act) attron(A_REVERSE);
            std::wstring w = to_wide(label);
            mvaddnwstr(0, col, w.c_str(),
                       std::min<int>(w.size(), std::max(0, width() - col)));
            if (act) attroff(A_REVERSE);
            col += display_cols(label);
        }
        attroff(COLOR_PAIR(P_STATUS));
    }

    void draw() {
        erase();
        draw_tabs();
        int view_h = chat_height();

        // Build the full display list = committed lines + live (uncommitted)
        // streaming buffer wrapped to the current width. The stream buffer is
        // rendered in place and only committed once complete.
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

    // A run of bar text sharing one color pair. Sequenced left-to-right to build
    // the status bar; each carries a drop-priority so we can shed segments when
    // the terminal is too narrow (higher priority = dropped first).
    struct Seg {
        std::string text;
        int pair = P_BANNER;
        int drop = 0;   // 0 = never drop; larger = shed sooner
    };

    static int display_cols(const std::string& s) {
        return tui::text::display_cols(s);
    }

    // Decode a UTF-8 byte string into Unicode code points. Used to render the
    // status bar via ncursesw's wide-char API (mvaddnwstr), which places each
    // glyph in one cell correctly. Writing raw UTF-8 bytes with the narrow
    // byte-counted mvaddnstr mis-splits multibyte sequences on some terminals
    // (PuTTY / macOS Terminal over SSH), producing letter/dash garbage.
    static std::wstring to_wide(const std::string& s) {
        return tui::text::to_wide(s);
    }

    static std::string kfmt(long n) { return agent::bar::kfmt(n); }

    static int gauge_pair(double f) {
        switch (agent::bar::pressure(f)) {
            case agent::bar::Pressure::Crit: return P_GAUGE_CRIT;
            case agent::bar::Pressure::Warn: return P_GAUGE_WARN;
            default:                         return P_GAUGE_OK;
        }
    }

    static std::string state_glyph(agent::RunState s) {
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

    static int state_pair(agent::RunState s) {
        using S = agent::RunState;
        switch (s) {
            case S::Thinking:  return P_GAUGE_WARN;
            case S::Streaming: return P_BAR_DIM;
            case S::Tooling:   return P_GAUGE_OK;
            case S::Error:     return P_GAUGE_CRIT;
            default:           return P_BANNER;
        }
    }

    // Assemble the ordered, colored segments for the bar (excluding the clock,
    // which is pinned right by the drawer). The bar is always fully featured:
    // every segment is present from launch, showing an em-dash placeholder when
    // its metric has no data yet, so the layout never shifts on first use.
    std::vector<Seg> bar_segments() const {
        std::vector<Seg> segs;
        segs.push_back({"[" + cfg_.model + "]", P_BANNER, 5});
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

    // Render the blue status bar, BitchX-style: ordered colored segments on the
    // left, a smooth context gauge, and an IRC clock pinned right. Sheds the
    // lowest-priority segments first when the terminal is too narrow. Cheap
    // enough to call once a second for the live clock tick.
    void draw_status_bar(const std::string& tail) {
        int w = width();
        int y = height() - 2;

        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char clk[16];
        std::strftime(clk, sizeof(clk), "[%H:%M:%S]", &tm);
        std::string clock = clk;
        int clock_w = display_cols(clock);

        // Paint the whole bar blue first.
        attron(COLOR_PAIR(P_BANNER));
        mvhline(y, 0, ' ', w);
        attroff(COLOR_PAIR(P_BANNER));

        std::vector<Seg> segs = bar_segments();

        // Context gauge: always shown when a window size is configured (which it
        // is by default). Renders an empty gauge until the first usage report.
        bool have_ctx = (cfg_.context_size > 0);
        long ctx_used = ctx_used_ >= 0 ? ctx_used_ : 0;
        double frac = have_ctx
                          ? static_cast<double>(ctx_used) / cfg_.context_size
                          : 0.0;

        // Reserve space for clock (right) + optional scroll tail.
        int right_w = clock_w + 1;
        int budget = w - right_w;
        if (budget < 0) budget = 0;

        // Shed highest-drop segments until the fixed text fits, keeping a
        // reasonable minimum for the gauge if we have one.
        int gauge_min = have_ctx ? 12 : 0;   // "ctx " + short bar + "%"
        auto text_cols = [&]() {
            int c = 0;
            for (auto& s : segs) c += display_cols(s.text);
            return c;
        };
        while (text_cols() + gauge_min > budget && !segs.empty()) {
            int worst = -1, worst_i = -1;
            for (size_t i = 0; i < segs.size(); ++i)
                if (segs[i].drop > worst) { worst = segs[i].drop; worst_i = (int)i; }
            if (worst <= 0) break;            // only never-drop segments remain
            segs.erase(segs.begin() + worst_i);
        }

        int x = 0;
        auto put = [&](const std::string& s, int pair) {
            if (x >= budget) return;
            std::wstring w = to_wide(s);
            int room = budget - x;
            if (static_cast<int>(w.size()) > room) w.resize(room);
            attron(COLOR_PAIR(pair));
            mvaddnwstr(y, x, w.c_str(), static_cast<int>(w.size()));
            attroff(COLOR_PAIR(pair));
            x += static_cast<int>(w.size());
            if (x > budget) x = budget;
        };

        for (auto& s : segs) put(s.text, s.pair);

        // Context gauge, colored by pressure, with sub-cell smooth fill.
        if (have_ctx && x < budget) {
            put("  ctx ", P_BAR_DIM);
            int cells = std::min(24, std::max(6, (budget - x) - 14));
            if (cells > 0 && x < budget) {
                put("\u2590", P_BAR_DIM);              // left edge
                put(agent::bar::gauge_bar(frac, cells), gauge_pair(frac));
                put("\u258c", P_BAR_DIM);              // right edge
                char b[48];
                std::snprintf(b, sizeof(b), " %d%% %s/%s",
                              static_cast<int>(frac * 100 + 0.5),
                              kfmt(ctx_used).c_str(),
                              kfmt(cfg_.context_size).c_str());
                put(b, gauge_pair(frac));
            }
        }

        // Scroll position tail, dim, if it still fits.
        if (!tail.empty() && x + display_cols(tail) + 1 < budget)
            put("  " + tail, P_BAR_DIM);

        // Pinned clock, right-aligned (ASCII, but keep the wide path uniform).
        if (clock_w < w) {
            std::wstring wc = to_wide(clock);
            attron(COLOR_PAIR(P_BAR_DIM));
            mvaddnwstr(y, w - clock_w, wc.c_str(), static_cast<int>(wc.size()));
            attroff(COLOR_PAIR(P_BAR_DIM));
        }
    }

    // Update only the status bar (for the once-per-second clock tick) without
    // rebuilding the whole scrollback view.
    void tick_clock() {
        int total = static_cast<int>(win().lines.size());
        if (!win().stream_buf.empty()) total += stream_lines();
        int start = std::min(win().scroll_top,
                             std::max(0, total - chat_height()));
        draw_status_bar("ln " + std::to_string(start + 1) + "/" +
                        std::to_string(total));
        refresh();
    }

    void draw_input(const std::string& s) {
        draw_drawer(s);   // render/erase the command drawer above the input
        int y = height() - 1;
        move(y, 0);
        clrtoeol();
        attron(COLOR_PAIR(P_USER));
        std::string shown = "prompt> " + s;
        mvaddnstr(y, 0, shown.c_str(), width());
        attroff(COLOR_PAIR(P_USER));
        refresh();
    }

    // ---- command drawer -------------------------------------------------

    // Open the drawer iff the input begins with '/'; reset selection when it
    // first opens. Closing it when the leading slash is gone.
    void update_drawer(const std::string& input) {
        bool want = drawer_wants_open(input);
        if (want && !drawer_open_) drawer_sel_ = 0;
        drawer_open_ = want;
    }

    static std::string drawer_token(const std::string& input) {
        return tui::palette::token(input);
    }

    static bool drawer_has_arg(const std::string& input) {
        return tui::palette::has_arg(input);
    }

    std::vector<const Command*> filter_commands(const std::string& token) {
        return tui::palette::filter(commands(), token);
    }

    bool drawer_wants_open(const std::string& input) const {
        return tui::palette::wants_open(input);
    }

    void draw_drawer(const std::string& input) {
        if (!drawer_open_) return;

        int bar_row = height() - 2;   // status bar sits here; drawer grows above
        std::string token = drawer_token(input);

        // When typing an argument, show one usage line for the matched command.
        std::vector<std::string> rows;
        std::vector<int> row_is_sel;   // parallel: 1 if this is the selectable cmd row
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

        // Clamp selection to the visible match count.
        int nsel = arg_mode ? 0 : static_cast<int>(matches.size());
        if (drawer_sel_ >= nsel) drawer_sel_ = std::max(0, nsel - 1);
        if (drawer_sel_ < 0) drawer_sel_ = 0;

        // Grow upward from just above the status bar, capped to available space.
        int max_rows = std::max(1, bar_row - chat_top());
        int header = 1;
        int shown = std::min<int>(rows.size(), max_rows - header);
        int top = bar_row - header - shown;

        // Header line.
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

    // Tab pressed with the drawer open: complete the command name. Returns the
    // (possibly rewritten) input line.
    std::string drawer_complete(const std::string& input) {
        return tui::palette::complete(commands(), input, drawer_sel_);
    }

    // A modal, PuTTY-safe selectable list that grows upward from the input line
    // (same visual language as the command drawer, no box chars). Blocks until
    // the user picks (Enter -> index) or cancels (Esc -> -1). Supports Up/Down
    // and PgUp/PgDn with a scrolling window over long lists.
    int drawer_menu(const std::string& title,
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

            // Repaint chat underneath so stale rows don't linger, then overlay.
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

    void send(const std::string& prompt) {
        agent::AgentHooks hooks;
        win().reason_buf.clear();
        win().reason_folded = false;
        show_reasoning_ = cfg_.show_reasoning;
        win().stream_ts = timestamp();   // stamp the reply the moment it starts
        hooks.on_reasoning = [this](const std::string& d) {
            // Live thinking: accumulate and render dim, above the answer.
            win().reason_buf += d;
            win().scroll_top = max_scroll();
            draw();
        };
        hooks.on_token = [this](const std::string& d) {
            // First answer token: fold any thinking into one collapsible summary
            // line so it stops occupying the viewport.
            if (!win().reason_folded && !win().reason_buf.empty()) {
                fold_reasoning();
            }
            // Accumulate the partial message and re-render it live in place.
            // Do NOT commit per token (that made each fragment its own line).
            win().stream_color = P_ASSISTANT;
            win().stream_buf += d;
            win().scroll_top = max_scroll();   // auto-follow
            draw();
        };
        hooks.on_assistant = [this](const std::string& s) {
            // Non-streaming path: nothing was streamed, so commit the whole msg.
            if (win().stream_buf.empty()) append_line(P_ASSISTANT, s);
        };
        hooks.on_status = [this](const std::string& s) { append_line(P_STATUS, s); };
        hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
            flush_stream();
            append_line(P_STATUS, "tool: " + n + " " + a.dump());
        };
        hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
            append_line(P_STATUS, "result:" + n + " " + (r.ok ? r.output : r.error));
        };
        hooks.on_state = [this](agent::RunState s) {
            state_ = s;
            draw();
        };
        hooks.on_stats = [this](const agent::Stats& s) {
            stats_ = s;
            if (s.prompt_tokens >= 0) ctx_used_ = s.prompt_tokens;
            draw();
        };
        try {
            // Reuse this window's persistent agent so context accumulates
            // across turns instead of starting fresh each prompt.
            win().agent->set_hooks(hooks);
            win().agent->run(prompt);
            win().dirty = true;
        } catch (const std::exception& e) {
            state_ = agent::RunState::Error;
            flush_stream();
            append_line(P_STATUS, std::string("error: ") + e.what());
        }
        if (state_ != agent::RunState::Error) state_ = agent::RunState::Idle;
        flush_stream();
        autosave();
        draw();
    }

    // Collapse the live thinking buffer into a single dim summary line. The
    // full reasoning is preserved in the telemetry log, not the viewport.
    void fold_reasoning() {
        if (win().reason_folded) return;
        win().reason_folded = true;
        if (win().reason_buf.empty()) return;
        size_t words = 1;
        for (char ch : win().reason_buf) if (ch == ' ') ++words;
        append_line_ts(P_REASONING,
                       "[thought for " + std::to_string(words) + " words]",
                       win().stream_ts.empty() ? timestamp() : win().stream_ts);
        win().reason_buf.clear();
    }

    void flush_stream() {
        // If we finished on pure thinking (no answer streamed), still fold it.
        if (!win().reason_folded && !win().reason_buf.empty()) fold_reasoning();
        if (win().stream_buf.empty()) return;
        append_line_ts(win().stream_color, win().stream_buf,
                       win().stream_ts.empty() ? timestamp() : win().stream_ts);
        win().stream_buf.clear();
        win().stream_ts.clear();
        draw();
    }

    // ---- session persistence -------------------------------------------

    // Build a Session snapshot from a window's agent history + metadata.
    agent::Session snapshot(Window& w) {
        agent::Session s;
        s.id = w.session_id;
        s.model = cfg_.model;
        s.messages = w.agent ? w.agent->history() : std::vector<agent::Message>{};
        s.derive_title();
        if (w.title != "chat" && !w.title.empty()) s.title = w.title;
        return s;
    }

    // Persist the active window silently (called after each turn). No-op if the
    // window has no real conversation yet.
    void autosave() {
        Window& w = win();
        if (!w.dirty || !w.agent || w.agent->history().empty()) return;
        agent::Session s = snapshot(w);
        if (store_.save(s)) {
            w.session_id = s.id;
            if (w.title == "chat" && !s.title.empty()) w.title = s.title;
            w.dirty = false;
        }
    }

    // Explicit /save: persist and report.
    void save_session() {
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

    // Load a stored session into a new window.
    void load_session(const std::string& id) {
        agent::Session s;
        if (!store_.load(id, s)) {
            append_line(P_STATUS, "load failed: no session " + id);
            return;
        }
        Window& w = new_window(s.title.empty() ? "chat" : s.title);
        w.session_id = s.id;
        w.agent->set_history(s.messages);
        // Replay the conversation into the scrollback so it's visible.
        for (const auto& m : s.messages) {
            if (m.role == "user") append_line(P_USER, "> " + m.content);
            else if (m.role == "assistant" && !m.content.empty())
                append_line(P_ASSISTANT, m.content);
        }
        append_line(P_STATUS, "loaded session " + s.id);
        draw();
    }

    // /load or /sessions picker.
    void pick_session() {
        auto metas = store_.list();
        if (metas.empty()) { append_line(P_STATUS, "no saved sessions"); return; }
        std::vector<std::string> labels;
        for (const auto& m : metas)
            labels.push_back(m.title + "  (" + std::to_string(m.message_count) +
                             " msgs)");
        int sel = drawer_menu("Load session", labels);
        if (sel >= 0 && sel < static_cast<int>(metas.size()))
            load_session(metas[sel].id);
    }

    // ---- window management ---------------------------------------------

    void switch_to(size_t idx) {
        if (idx >= windows_.size() || idx == active_) return;
        active_ = idx;
        draw();
    }

    void close_window() {
        if (windows_.size() <= 1) {
            append_line(P_STATUS, "cannot close the last window");
            return;
        }
        autosave();
        windows_.erase(windows_.begin() + active_);
        if (active_ >= windows_.size()) active_ = windows_.size() - 1;
        draw();
    }

    // ---- slash command framework ---------------------------------------
    //
    // Adding a command: append one Command entry to build_commands(). The table
    // is the single source of truth for dispatch, `/help`, and tab-less usage
    // hints, so a new command automatically appears in the help listing.

    // Built lazily so handlers can capture `this`. Order here is the order shown
    // in /help.
    const std::vector<Command>& commands() {
        if (commands_.empty()) build_commands();
        return commands_;
    }

    void build_commands() {
        commands_ = {
            {"help", {"?", "h"}, "[command]",
             "list commands, or show detail for one",
             [this](const std::string& a) { cmd_help(a); }},
            {"window", {"win", "w"}, "new|close|list|rename <name>",
             "manage chat windows",
             [this](const std::string& a) { cmd_window(a); }},
            {"save", {}, "",
             "persist the current conversation",
             [this](const std::string&) { save_session(); }},
            {"load", {"sessions", "open"}, "",
             "pick a saved session to reopen",
             [this](const std::string&) { pick_session(); }},
            {"quit", {"exit", "q"}, "",
             "save all windows and exit",
             [this](const std::string&) { request_quit(); }},
        };
    }

    // Find a command by name or alias (both given without the leading slash).
    const Command* find_command(const std::string& name) {
        return tui::palette::find(commands(), name);
    }

    // Parse a slash command from the input line. Returns true if handled (so the
    // caller should not treat it as a prompt).
    bool handle_slash(const std::string& line) {
        if (line.empty() || line[0] != '/') return false;
        std::string rest = line.substr(1);
        std::string name, arg;
        size_t sp = rest.find(' ');
        if (sp == std::string::npos) name = rest;
        else { name = rest.substr(0, sp); arg = rest.substr(sp + 1); }

        const Command* c = find_command(name);
        if (!c) {
            append_line(P_STATUS,
                        "unknown command: /" + name + "  (try /help)");
            return true;
        }
        c->run(arg);
        return true;
    }

    // Canonical "/name  <args>" spelling for help/usage lines.
    std::string usage(const Command& c) const {
        return tui::palette::usage(c);
    }

    // /help with no arg lists every command; with an arg shows detail + aliases.
    // Output goes to the scrollback (PuTTY-safe, scrollable, no box decor).
    void cmd_help(const std::string& arg) {
        if (arg.empty()) {
            banner("Slash commands (type /help <command> for detail):");
            size_t w = 0;
            for (const auto& c : commands()) w = std::max(w, usage(c).size());
            for (const auto& c : commands()) {
                std::string u = usage(c);
                u.append(w - u.size() + 2, ' ');
                append_line(P_STATUS, "  " + u + c.help);
            }
            draw();
            return;
        }
        std::string name = arg;
        if (!name.empty() && name[0] == '/') name = name.substr(1);
        const Command* c = find_command(name);
        if (!c) { append_line(P_STATUS, "no such command: /" + name); return; }
        banner(usage(*c));
        append_line(P_STATUS, "  " + c->help);
        if (!c->aliases.empty()) {
            std::string al = "  aliases:";
            for (const auto& a : c->aliases) al += " /" + a;
            append_line(P_STATUS, al);
        }
        draw();
    }

    void cmd_window(const std::string& arg) {
        if (arg == "new") { new_window("chat"); draw(); }
        else if (arg == "close") { close_window(); }
        else if (arg == "list") {
            std::string s = "windows:";
            for (size_t i = 0; i < windows_.size(); ++i)
                s += " " + std::to_string(i + 1) + ":" + windows_[i]->title +
                     (i == active_ ? "*" : "");
            append_line(P_STATUS, s);
        } else if (arg.rfind("rename ", 0) == 0) {
            win().title = arg.substr(7);
            append_line(P_STATUS, "renamed window to " + win().title);
            draw();
        } else {
            append_line(P_STATUS, "usage: /window new|close|list|rename <name>");
        }
    }

    void request_quit() { quit_ = true; }

    void help_screen() {
        info_dialog("Help", {
            "F1        show this help",
            "F2        show configuration",
            "F3        toggle live thinking display",
            "F10       server settings (URL, token, model)",
            "Enter     send the prompt",
            "Ctrl-G    send the prompt",
            "PgUp/PgDn scroll scrollback",
            "Ctrl-N    new window        Ctrl-W  close window",
            "Alt+1..9  switch to window N",
            "Ctrl-C    quit",
            "",
            "Type '/' to open the command drawer: filter as you type,",
            "Tab completes, Up/Down select, Enter runs, Esc closes.",
            "Type /help for the full command list, /help <cmd> for detail.",
            "",
            "Tools: read (paginated), write (patch), search (grep/semantic).",
        });
    }

    void config_screen() {
        auto mask = [](const std::string& s) {
            return s.empty() ? std::string("(unset)") : std::string(s.size(), '*');
        };
        info_dialog("Configuration", {
            "api_base:  " + cfg_.api_base,
            "api_key:   " + mask(cfg_.api_key),
            "model:     " + cfg_.model,
            "stream:    " + std::string(cfg_.stream ? "on" : "off"),
            "context:   " + std::to_string(cfg_.context_size) + " tokens" +
                (last_detected_.ok ? " (auto-detected)" : ""),
            "max_iter:  " + std::to_string(cfg_.max_tool_iterations),
            "system:    " + cfg_.system_prompt_path,
            "tools:     " + cfg_.tools_prompt_path,
        });
    }

    // Probe the server's /v1/models endpoint and fill in model / context size.
    // When `force` is true, values are shown even if unchanged; otherwise only a
    // concise status line is added. Never blocks the UI for more than a few
    // seconds (curl timeouts in the library).
    void detect_server(bool force) {
        agent::ServerInfo info = agent::apply_server_autodetect(cfg_);
        if (!info.ok) {
            if (force)
                append_line(P_STATUS, "detect: server unreachable at " +
                                          cfg_.api_base);
            return;
        }
        last_detected_ = info;
        std::string note = "detected model=" + cfg_.model +
                           " n_ctx=" + std::to_string(cfg_.context_size);
        if (info.context_train > 0 &&
            info.context_train != cfg_.context_size)
            note += " (max " + std::to_string(info.context_train) + ")";
        append_line(P_STATUS, note);
        draw();
    }

    // Server settings using a native libform dialog. The detected server info
    // (if any) is shown so the user knows what auto-detection found.
    void settings_screen() {
        std::string det = last_detected_.ok
            ? ("detected: " + last_detected_.model + " / n_ctx " +
               std::to_string(last_detected_.context_size))
            : std::string("detected: (none - press Detect below)");

        std::vector<std::string> pre = {"Edit settings",
                                        "Detect from server now"};
        int pick = menu_select(det, pre);
        if (pick == 1) { detect_server(true); return; }
        if (pick != 0) return;

        std::vector<FieldSpec> fields = {
            {"Server URL", cfg_.api_base, false},
            {"Token", cfg_.api_key, true},
            {"Model (blank = auto)", cfg_.model, false},
            {"Context n_ctx (0 = auto)", std::to_string(cfg_.context_size), false},
        };
        if (!form_edit("Server settings", fields)) return;

        cfg_.api_base = fields[0].value;
        cfg_.api_key = fields[1].value;
        // Blank model / zero context mean "let auto-detect decide": clear the
        // explicit flag so the next probe fills them.
        if (fields[2].value.empty()) {
            cfg_.model_explicit = false;
        } else {
            cfg_.model = fields[2].value;
            cfg_.model_explicit = true;
        }
        try {
            int n = std::stoi(fields[3].value);
            if (n > 0) { cfg_.context_size = n; cfg_.context_explicit = true; }
            else       { cfg_.context_explicit = false; }
        } catch (...) {}

        // Re-probe to fill anything the user left on auto.
        if (!cfg_.model_explicit || !cfg_.context_explicit)
            detect_server(false);

        std::vector<std::string> post = {"Save to " + settings_path_,
                                         "Apply only (don't save)"};
        int choice = menu_select("Apply settings?", post);
        if (choice == 0) {
            save_settings();
            append_line(P_STATUS, "settings saved to " + settings_path_);
        } else if (choice == 1) {
            append_line(P_STATUS, "settings applied (not saved)");
        }
    }

    void save_settings() {
        std::ofstream f(settings_path_, std::ios::trunc);
        if (!f) return;
        f << "# amber settings\n";
        f << "api_base=" << cfg_.api_base << "\n";
        f << "api_key=" << cfg_.api_key << "\n";
        f << "model=" << cfg_.model << "\n";
        f << "context_size=" << cfg_.context_size << "\n";
        f << "system_prompt=" << cfg_.system_prompt_path << "\n";
        f << "tools_prompt=" << cfg_.tools_prompt_path << "\n";
    }

    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    agent::SessionStore store_;
    std::string settings_path_ = "amber.conf";

    // ---- windows (IRC-style switchable conversations) -------------------
    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;

    // Accessors for the active window's state (all former per-window members
    // now live on Window).
    Window& win() { return *windows_[active_]; }
    const Window& win() const { return *windows_[active_]; }

    std::vector<Command> commands_;  // built lazily by build_commands()
    bool quit_ = false;              // set by /quit; breaks the input loop

    // ---- command drawer (grows upward from the input line) --------------
    // A PuTTY-safe list panel drawn directly on stdscr (no ACS box chars). It
    // opens when '/' is the first character of the input line and offers live
    // filtering + Tab completion of slash commands.
    bool drawer_open_ = false;
    int drawer_sel_ = 0;             // selected row within the filtered list

    bool show_reasoning_ = true;    // toggle live thinking display

    // ---- BitchX-style status bar state ----------------------------------
    agent::RunState state_ = agent::RunState::Idle;  // live activity
    agent::Stats stats_;            // last-request telemetry (latency/tps/tokens)
    long ctx_used_ = -1;            // prompt_tokens of the last request
    agent::ServerInfo last_detected_;  // most recent /v1/models probe result
};

} // namespace

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string config_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (a == "--api-base" && i + 1 < argc) cfg.api_base = argv[++i];
        else if (a == "--api-key" && i + 1 < argc) cfg.api_key = argv[++i];
        else if (a == "--model" && i + 1 < argc) { cfg.model = argv[++i]; cfg.model_explicit = true; }
        else if (a == "--system" && i + 1 < argc) cfg.system_prompt_path = argv[++i];
        else if (a == "--tools" && i + 1 < argc) cfg.tools_prompt_path = argv[++i];
        else if (a == "--no-stream") cfg.stream = false;
    }
    if (!config_file.empty()) cfg.load(config_file);
    {
        std::ifstream sf("amber.conf");
        if (sf) cfg.load("amber.conf");
    }
    cfg.apply_environment();

    if (auto errs = cfg.validate(); !errs.empty()) {
        std::fprintf(stderr, "error: invalid configuration:\n");
        for (const auto& e : errs)
            std::fprintf(stderr, "  - %s\n", e.c_str());
        return 2;
    }

    if (cfg.system_prompt_path.empty()) cfg.system_prompt_path = "prompts/system.md";
    if (cfg.tools_prompt_path.empty()) cfg.tools_prompt_path = "prompts/tools.md";

    agent::ToolRegistry registry;
    agent::register_default_tools(registry);

    Tui tui(cfg, registry);
    tui.run();
    return 0;
}
