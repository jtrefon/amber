// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"
#include "welcome.h"

#include <algorithm>
#include <cmath>
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
    // Count wrapped screen lines from committed rich lines.
    // Each rich::Line may wrap to multiple screen rows.
    int total = 0;
    for (const auto& line : win().lines)
        total += static_cast<int>(rich::wrap(line, width()).size());
    total += stream_lines();
    int m = total - chat_height();
    return m < 0 ? 0 : m;
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
    std::strftime(buf, sizeof(buf), "[%H:%M:%S] ", &tm);
    return buf;
}
void Tui::append_line(int color, const std::string& text) {
    append_line_ts(color, text, timestamp());
}
void Tui::append_line_ts(int color, const std::string& text,
                         const std::string& ts) {
    // Build one RichLine with a dim timestamp run followed by the body run,
    // then wrap it to the current width so wrapped continuations align.
    rich::Line head;
    if (!ts.empty())
        head.runs.push_back({ts, P_REASONING, false, true});  // faint timestamp
    rich::Run body; body.pair = color; body.text = text;
    head.runs.push_back(body);
    auto wrapped = rich::wrap(head, width());
    for (auto& l : wrapped) win().lines.push_back(std::move(l));
    trim_lines();
    int max = max_scroll();
    if (win().scroll_top >= max - 2)
        win().scroll_top = max;
}
void Tui::append_rich(const rich::Line& l) {
    win().lines.push_back(l);
    trim_lines();
    win().scroll_top = max_scroll();
}
void Tui::append_markdown(const std::string& md) {
    if (win().markdown_on) {
        // Render the assistant reply as Markdown, then prepend a faint
        // timestamp run to the first rendered line so it stays on the same
        // line as the reply (matching user/tool/status lines) instead of
        // floating on its own row above a blank gap.
        auto lines = md::render(md, md_style_);
        if (!lines.empty()) {
            rich::Run ts;
            ts.text = (win().stream_ts.empty() ? timestamp()
                                               : win().stream_ts) + " ";
            ts.pair = P_REASONING;
            ts.dim = true;
            lines.front().runs.insert(lines.front().runs.begin(),
                                      std::move(ts));
            for (auto& l : lines) win().lines.push_back(std::move(l));
        }
    } else {
        append_line(P_ASSISTANT, md);
    }
    trim_lines();
    win().scroll_top = max_scroll();
}
void Tui::banner(const std::string& text) {
    rich::Line l;
    rich::Run r; r.pair = P_BANNER; r.bold = true; r.text = text;
    l.runs.push_back(r);
    win().lines.push_back(std::move(l));
    win().scroll_top = max_scroll();
}
void Tui::append_rich_to(std::vector<rich::Line>& view, const std::string& text,
                         int color, int w) {
    rich::Line l;
    rich::Run r; r.pair = color; r.text = text;
    l.runs.push_back(r);
    for (auto& x : rich::wrap(l, w)) view.push_back(std::move(x));
}
void Tui::trim_lines() {
    if (win().lines.size() > 10000)
        win().lines.erase(win().lines.begin(),
                          win().lines.begin() + 5000);
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

std::vector<Tui::Seg> Tui::bar_segments() const {
    std::vector<Seg> segs;
    std::string wtag = "[" + std::to_string(active_ + 1) + "/" +
                       std::to_string(windows_.size()) + "]";
    segs.push_back({wtag, P_BANNER, 3});
    segs.push_back({" [" + cfg_.model + "]", P_BAR_DIM | A_BOLD, 5});

    // Agent mode label with full words and colour coding.
    std::string mode_txt;
    int mode_pair = P_BAR_DIM;
    switch (cfg_.mode) {
        case agent::AgentMode::Read:
            mode_txt = " read ";
            mode_pair = P_GAUGE_OK;      // green on blue — safe
            break;
        case agent::AgentMode::Write:
            mode_txt = " write ";
            mode_pair = P_GAUGE_WARN;    // yellow on blue — cautious
            break;
        case agent::AgentMode::Yolo:
            mode_txt = " yolo ";
            mode_pair = P_BUTTON_ACT;    // white on yellow — most prominent
            break;
    }
    segs.push_back({mode_txt, mode_pair, 2});

    if (scroll_mode_) {
        segs.push_back({" S ", P_GAUGE_OK, 0});
    }

    if (stats_.latency_ms >= 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  lag %.0fms", stats_.latency_ms);
        int lag_pair = stats_.latency_ms > 5000 ? P_GAUGE_CRIT
                     : stats_.latency_ms > 1000 ? P_GAUGE_WARN
                     : P_BAR_DIM;
        segs.push_back({b, lag_pair, 6});
    } else {
        segs.push_back({"  lag " + std::string(text::glyph::emdash()), P_BAR_DIM, 6});
    }
    if (stats_.tps > 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  %.0f t/s", stats_.tps);
        segs.push_back({b, P_BAR_DIM, 4});
    } else {
        segs.push_back({"  " + std::string(text::glyph::emdash()) + " t/s", P_BAR_DIM, 4});
    }
    std::string up = stats_.prompt_tokens >= 0 ? kfmt(stats_.prompt_tokens)
                                               : text::glyph::emdash();
    std::string dn = stats_.completion_tokens >= 0
                         ? kfmt(stats_.completion_tokens) : text::glyph::emdash();
    segs.push_back({"  " + std::string(text::glyph::up()) + up + " " +
                    text::glyph::down() + dn, P_BAR_DIM, 7});

    // Background jobs: count plus a live countdown to the nearest idle/hard
    // deadline so the user can see a server will be auto-reaped soon.
    int njobs = jobs_.running_count();
    if (njobs > 0) {
        std::string s = "  " + std::to_string(njobs) + " job" +
                        (njobs > 1 ? "s" : "");
        int rem = jobs_.min_timeout_remaining();
        if (rem >= 0) s += " " + std::to_string(rem) + "s";
        segs.push_back({s, P_GAUGE_WARN, 1});
    } else if (!running_tool_.empty()) {
        // A synchronous tool (e.g. bash) is executing on the agent worker; it
        // is not a JobService background job but the user should still see it
        // running here rather than the bar looking idle.
        segs.push_back({"  " + running_tool_ + "…", P_GAUGE_WARN, 1});
    }
    return segs;
}

void Tui::draw() {
    dirty_ = true;
    if (win().welcome_art) {
        erase();
        welcome::render(stdscr, chat_top(), width());
        draw_status_bar("welcome");
        wnoutrefresh(stdscr);
        return;
    }

    // Assemble the full scrollback (committed lines + live reasoning/stream)
    // as RichLines, then render through the dedicated chat canvas. The canvas
    // owns width-aware wrapping and viewport scrolling.
    std::vector<rich::Line> view = win().lines;

    if (show_reasoning_ && !win().reason_folded && !win().reason_buf.empty()) {
        rich::Line label;
        rich::Run r0; r0.pair = P_REASONING; r0.dim = true;
        r0.text = "thinking...";
        label.runs.push_back(r0);
        view.push_back(label);
        rich::Line body;
        rich::Run r1; r1.pair = P_REASONING; r1.dim = true; r1.text = win().reason_buf;
        body.runs.push_back(r1);
        for (auto& l : rich::wrap(body, width())) view.push_back(std::move(l));
    }
    if (!win().stream_buf.empty()) {
        if (win().markdown_on) {
            auto preview = md::render(win().stream_buf, md_style_);
            if (!preview.empty()) {
                rich::Run ts;
                ts.text = (win().stream_ts.empty() ? timestamp()
                                                   : win().stream_ts) + " ";
                ts.pair = P_REASONING;
                ts.dim = true;
                preview.front().runs.insert(preview.front().runs.begin(),
                                            std::move(ts));
                for (auto& l : preview) view.push_back(std::move(l));
            }
        } else {
            append_rich_to(view, win().stream_buf, win().stream_color, width());
        }
    }

    chat_canvas_.resize(chat_top(), chat_height(), width());
    chat_canvas_.set_lines(view);
    if (static_cast<size_t>(win().scroll_top) >
        static_cast<size_t>(chat_canvas_.max_top()))
        win().scroll_top = chat_canvas_.max_top();
    chat_canvas_.set_top(win().scroll_top);
    chat_canvas_.render();

    {
        int total = chat_canvas_.wrapped_count();
        int pos = win().scroll_top;
        int vis = chat_height();
        std::string scroll_glyph;
        if (total > vis) {
            int pct = 100 - static_cast<int>(100.0 * std::min(pos, total - vis)
                                             / (total - vis));
            scroll_glyph = " P:" + std::to_string(pct) + "%";
        }
        draw_status_bar(scroll_glyph);
    }
    wnoutrefresh(stdscr);
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

    // Width of the framed activity indicator ([ |...| ]) that is right-justified
    // just left of the clock. Segments must stop before it so they are never
    // overwritten by it.
    constexpr int kIW = 12;
    int activity_w = kIW + 1;  // indicator + a leading gap

    attron(COLOR_PAIR(P_BANNER));
    mvhline(y, 0, ' ', w);
    attroff(COLOR_PAIR(P_BANNER));

    std::vector<Seg> segs = bar_segments();

    bool have_ctx = (cfg_.context_size > 0);
    long ctx_used = ctx_used_ >= 0 ? ctx_used_ + live_ctx_offset_ : live_ctx_offset_;
    double frac = have_ctx
                      ? static_cast<double>(ctx_used) / cfg_.context_size
                      : 0.0;

    // Reserve the clock and the activity indicator so the jobs segment (added
    // last, hence rightmost) is never drawn under the indicator.
    int right_w = clock_w + 1 + activity_w;
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
            put(text::glyph::block_l(), P_BAR_DIM);
            std::string bar = text::glyph::utf8()
                                  ? agent::bar::gauge_bar(frac, cells)
                                  : agent::bar::gauge_bar_ascii(frac, cells);
            put(bar, gauge_pair(frac));
            put(text::glyph::block_r(), P_BAR_DIM);
            char b[48];
            std::snprintf(b, sizeof(b), " %d%% %s/%s",
                          static_cast<int>(std::lround(frac * 100)),
                          kfmt(ctx_used).c_str(),
                          kfmt(cfg_.context_size).c_str());
            put(b, gauge_pair(frac));
        }
    }

    if (!tail.empty() && x + display_cols(tail) + 1 < budget)
        put("  " + tail, P_BAR_DIM);

    // Framed activity indicator, right-justified before the clock.
    // wattron/wattroff used throughout (not chtype |) for reliable colour
    // pair rendering in ncursesw.
    int ix = w - clock_w - kIW - 1;  // one space left of the clock
    if (ix > x + 4) {
        wattron(stdscr, COLOR_PAIR(P_BAR_DIM));
        mvaddch(y, ix, '[');
        if (agent_busy_.load()) {
            // Advance phase at ~150ms intervals (~6 fps) so the wave
            // travels at a pleasant, observable pace.
            static auto last_phase = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_phase > std::chrono::milliseconds(150)) {
                ++anim_phase_;
                last_phase = now;
            }
            for (int i = 0; i < kIW - 2; ++i) {
                int c = anim_phase_ % 16;
                if (c >= 8) c = 16 - c;
                int d = std::abs(i - c);
                chtype a = A_NORMAL;
                if (d == 0)      a = A_BOLD;
                else if (d > 2)  a = A_DIM;
                attron(a);
                mvaddch(y, ix + 1 + i, '|');
                attroff(a);
            }
        } else {
            anim_phase_ = 0;
            mvaddstr(y, ix + 1, "   idle   ");  // 10 chars, centered
        }
        mvaddch(y, ix + kIW - 1, ']');
        wattroff(stdscr, COLOR_PAIR(P_BAR_DIM));
    }

    if (clock_w < w) {
        std::wstring wc = to_wide(clock);
        attron(COLOR_PAIR(P_BAR_DIM));
        mvaddnwstr(y, w - clock_w, wc.c_str(), static_cast<int>(wc.size()));
        attroff(COLOR_PAIR(P_BAR_DIM));
    }
}

void Tui::tick_clock() {
    dirty_ = true;
    int total = max_scroll() + chat_height();
    int vis = chat_height();
    int pos = win().scroll_top;
    std::string tail;
    if (total > vis) {
        int pct = 100 - static_cast<int>(100.0 * std::min(pos, total - vis)
                                         / (total - vis));
        tail = " P:" + std::to_string(pct) + "%";
    }
    draw_status_bar(tail);
    wnoutrefresh(stdscr);
}

void Tui::draw_input(const std::string& s, size_t cursor, const std::string& shadow) {
    dirty_ = true;
    draw_drawer(s);
    int y = height() - 1;
    int w = width();
    move(y, 0);
    clrtoeol();
    attron(COLOR_PAIR(P_USER));
    const std::string kPrompt = "amber> ";
    std::string shown = kPrompt + s;
    // If we have shadow text, include it for width calculation but render it faded.
    std::string full = shown + shadow;
    int prompt_w = static_cast<int>(kPrompt.size());
    int total_w = display_cols(full);
    // Cursor is after prompt + input text before cursor.
    std::string before_cursor = kPrompt + s.substr(0, std::min(cursor, s.size()));
    int cursor_col = display_cols(before_cursor);
    // Horizontal scrolling.
    int scroll_off = 0;
    if (cursor_col >= w) scroll_off = cursor_col - w + 1;
    if (scroll_off < prompt_w) scroll_off = 0;
    int vis_w = std::min(total_w - scroll_off, w);
    if (vis_w <= 0) { scroll_off = std::max(0, total_w - w); vis_w = std::min(total_w, w); }
    // Render visible portion of input text.
    std::string visible;
    if (scroll_off < static_cast<int>(shown.size()))
        visible = shown.substr(static_cast<size_t>(scroll_off),
                                static_cast<size_t>(vis_w));
    if (scroll_off > 0 && !visible.empty())
        visible[0] = '~';
    mvaddnstr(y, 0, visible.c_str(), std::min(vis_w, w));
    attroff(COLOR_PAIR(P_USER));
    // Render shadow (faded) after the input text.
    if (!shadow.empty() && cursor == s.size()) {
        // Only show shadow when cursor is at end of input.
        int input_w = display_cols(shown);
        int shadow_start = input_w - scroll_off;
        if (shadow_start >= 0 && shadow_start < w) {
            attron(A_DIM | COLOR_PAIR(P_GRAY));
            mvaddnstr(y, shadow_start, shadow.c_str(),
                       std::min(static_cast<int>(shadow.size()), w - shadow_start));
            attroff(A_DIM | COLOR_PAIR(P_GRAY));
        }
    }
    // Position cursor.
    int cx = cursor_col - scroll_off;
    if (cx < 0) cx = 0;
    if (cx >= w) cx = w - 1;
    curs_set(1);
    move(y, cx);
    wnoutrefresh(stdscr);
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

void Tui::draw_drawer(const std::string& input) {
    if (!drawer_open_) return;
    // Don't draw the drawer when a modal dialog is active — it uses raw
    // move/addch which bypasses the panel system and paints on top of
    // modal dialogs (approval, info, menu_select). Clear the area to
    // remove any stale text that would peek through the modal.
    if (modal_open_) {
        int bar_row = height() - 2;
        for (int row = std::max(0, bar_row - 8); row < bar_row; ++row) {
            move(row, 0); clrtoeol();
        }
        return;
    }

    int bar_row = height() - 2;
    std::string token = drawer_token(input);
    bool arg_mode = drawer_has_arg(input);
    std::vector<std::string> rows;

    auto append_choices = [&](std::string& line, const std::string& key) {
        const auto& ch = settings_.choices_for(key);
        if (!ch.empty()) {
            line += "  [";
            for (size_t i = 0; i < ch.size(); ++i) {
                if (i > 0) line += "|";
                line += ch[i];
            }
            line += "]";
        } else {
            double rlo, rhi;
            if (settings_.range_for(key, rlo, rhi))
                line += "  [" + std::to_string((int)rlo) + "-" + std::to_string((int)rhi) + "]";
        }
    };

    // Check if we're inside a command namespace.
    // Extract the namespace path: "/get policy mode" → "get.policy"
    std::string ns_path;
    size_t sp = input.find(' ', 1);
    if (sp != std::string::npos) {
        ns_path = input.substr(1);  // skip /
        // Strip trailing spaces so rfind finds the real word boundary.
        while (!ns_path.empty() && ns_path.back() == ' ')
            ns_path.pop_back();
        std::string partial;
        size_t last_sp = ns_path.rfind(' ');
        if (last_sp != std::string::npos) {
            partial = ns_path.substr(last_sp + 1);
            ns_path.resize(last_sp);
        }
        // Convert space-separated to dotted: "get policy" → "get.policy"
        std::string dotted;
        size_t p = 0;
        while (p < ns_path.size()) {
            size_t spc = ns_path.find(' ', p);
            if (spc == std::string::npos) { dotted += ns_path.substr(p); break; }
            if (!dotted.empty()) dotted += ".";
            dotted += ns_path.substr(p, spc - p);
            p = spc + 1;
        }
        // Strip command prefix (get/set) for registry lookup.
        // The JSON loader strips the first display-path component, so
        // "get.policy" is stored as "policy"; "get.detection.loop" as
        // "detection.loop".
        std::string lookup_key;
        size_t first_dot = dotted.find('.');
        if (first_dot != std::string::npos)
            lookup_key = dotted.substr(first_dot + 1);
        else
            lookup_key = dotted;
        // Children stored bare only when lookup_key matches the top-level
        // command name (e.g. "get" → children stored as "policy", not
        // "get.policy"). Nested namespaces include the full dotted path.
        bool is_top_cmd = (lookup_key == token);
        auto kids = settings_.children_of(lookup_key);
        if (!kids.empty()) {
            // If partial exactly matches a child that has its own children,
            // descend into that child's namespace.
            if (!partial.empty()) {
                std::string sub_key = is_top_cmd ? partial
                                                 : lookup_key + "." + partial;
                auto sub = settings_.children_of(sub_key);
                if (!sub.empty()) {
                    for (const auto& sk : sub) {
                        std::string full_key = sub_key + "." + sk;
                        std::string h = settings_.help_for(full_key);
                        std::string line = "  " + sk;
                        if (!h.empty()) {
                            if (sk.size() < 34) line.append(34 - sk.size(), ' ');
                            line += "  " + h;
                        }
                        append_choices(line, full_key);
                        rows.push_back(line);
                    }
                    goto render;
                }
            }
            for (const auto& k : kids) {
                if (!partial.empty() && k.rfind(partial, 0) != 0) continue;
                std::string full_key = is_top_cmd ? k : lookup_key + "." + k;
                std::string h = settings_.help_for(full_key);
                std::string line = "  " + k;
                if (!h.empty()) {
                    if (k.size() < 34) line.append(34 - k.size(), ' ');
                    line += "  " + h;
                }
                append_choices(line, full_key);
                rows.push_back(line);
            }
            if (rows.empty())
                rows.emplace_back("  (no matching option  -  Esc to cancel)");
            goto render;
        }
        // Check if we're at a non-namespace command with args (show usage).
        std::vector<const Command*> cmds = filter_commands(token);
        const Command* c = cmds.empty() ? nullptr : cmds.front();
        if (c) rows.push_back("  " + usage(*c) + "   " + c->help);
        else rows.emplace_back("  (no such command)");
        goto render;
    }

    // No space yet: show flat command list, or namespace children if the
    // token itself is a command with children in the JSON.
    {
        auto kids = settings_.children_of(token);
        if (!kids.empty()) {
            bool is_top = (token == "get" || token == "set");
            for (const auto& k : kids) {
                std::string full_key = is_top ? k : token + "." + k;
                std::string h = settings_.help_for(full_key);
                std::string line = "  " + k;
                if (!h.empty()) {
                    if (k.size() < 34) line.append(34 - k.size(), ' ');
                    line += "  " + h;
                }
                append_choices(line, full_key);
                rows.push_back(line);
            }
            if (rows.empty())
                rows.emplace_back("  (no matching option  -  Esc to cancel)");
            goto render;
        }
        // Fall back to flat command list.
        auto matches = filter_commands(token);
        for (auto* c : matches) {
            std::string u = usage(*c);
            if (u.size() < 34) u.append(34 - u.size(), ' ');
            rows.push_back("  " + u + "  " + c->help);
        }
        if (rows.empty()) rows.emplace_back("  (no matching command  -  Esc to cancel)");
    }

render:
    int nsel = arg_mode ? 0 : static_cast<int>(rows.size());
    if (drawer_sel_ >= nsel) drawer_sel_ = std::max(0, nsel - 1);
    if (drawer_sel_ < 0) drawer_sel_ = 0;

    int max_rows = std::max(1, bar_row - chat_top());
    int header = 1;
    int shown = std::min<int>(rows.size(), max_rows - header);
    int top = bar_row - header - shown;

    for (int row = top; row < bar_row; ++row) {
        move(row, 0);
        clrtoeol();
    }

    std::string hdr = " options  (Tab complete  Up/Down select  Enter run  ? help  Esc cancel) ";
    move(top, 0);
    attron(COLOR_PAIR(P_STATUS) | A_BOLD);
    for (int i = 0; i < width(); ++i) addch(' ');
    mvaddnstr(top, 0, hdr.c_str(), width());
    attroff(COLOR_PAIR(P_STATUS) | A_BOLD);

    for (int i = 0; i < shown; ++i) {
        int y = top + header + i;
        bool sel = (!arg_mode && i == drawer_sel_);
        if (sel) {
            attron(A_REVERSE);
            mvaddnstr(y, 0, rows[i].c_str(), width());
            attroff(A_REVERSE);
        } else {
            attron(COLOR_PAIR(P_ASSISTANT));
            mvaddnstr(y, 0, rows[i].c_str(), width());
            attroff(COLOR_PAIR(P_ASSISTANT));
        }
    }
}

} // namespace tui
