#include "render_engine.h"
#include "tui.h"
#include "welcome.h"
#include "tool_display.h"

#include "tui/drawer_rows.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <unistd.h>

namespace tui {

RenderEngine::RenderEngine(Tui& tui) : tui_(tui) {}

void RenderEngine::mark_working() noexcept {
    working_since_ = std::chrono::steady_clock::now();
    working_visible_ = true;
}

int RenderEngine::height() const { int y, x; getmaxyx(stdscr, y, x); (void)x; return y; }
int RenderEngine::width() const { int y, x; getmaxyx(stdscr, y, x); (void)y; return x; }
int RenderEngine::chat_top() const { return 0; }
int RenderEngine::chat_height() const { return std::max(1, height() - 2); }
int RenderEngine::lines_per_page() const { return chat_height(); }

std::vector<rich::Line> RenderEngine::build_view_without_working(const Window& w) const {
    std::vector<rich::Line> view = w.lines;
    if (show_reasoning_ && !w.reason_folded && !w.reason_buf.empty()) {
        rich::Line label;
        rich::Run r0; r0.pair = P_REASONING; r0.dim = true;
        r0.text = "thinking...";
        label.runs.push_back(r0);
        view.push_back(label);
        rich::Line body;
        rich::Run r1; r1.pair = P_REASONING; r1.dim = true; r1.text = w.reason_buf;
        body.runs.push_back(r1);
        for (auto& l : rich::wrap(body, width())) view.push_back(std::move(l));
        if (!w.stream_buf.empty())
            view.push_back(rich::Line{});
    }
    if (!w.stream_buf.empty()) {
        if (w.markdown_on) {
            auto preview = md::render(w.stream_buf, md_style_);
            if (!preview.empty()) {
                rich::Run ts;
                ts.text = (w.stream_ts.empty() ? Tui::timestamp() : w.stream_ts) + " ";
                ts.pair = P_REASONING;
                ts.dim = true;
                preview.front().runs.insert(preview.front().runs.begin(), std::move(ts));
                for (auto& l : preview) view.push_back(std::move(l));
            }
        } else {
            append_rich_to(view, w.stream_buf, w.stream_color, width());
        }
    }
    bool live = tui_.router_->busy() && (!w.stream_buf.empty() || !w.reason_buf.empty());
    if (live) {
        if (view.empty() || !view.back().runs.empty())
            view.push_back(rich::Line{});
    }
    {
        std::vector<rich::Line> fixed;
        fixed.reserve(view.size() + 4);
        for (size_t i = 0; i < view.size(); ++i) {
            if (view[i].is_hr) {
                if (!fixed.empty() && !fixed.back().runs.empty())
                    fixed.push_back(rich::Line{});
                fixed.push_back(view[i]);
                bool next_is_blank = (i + 1 < view.size() && view[i + 1].runs.empty());
                if (!next_is_blank)
                    fixed.push_back(rich::Line{});
            } else {
                fixed.push_back(view[i]);
            }
        }
        view.swap(fixed);
    }
    return view;
}

std::vector<rich::Line> RenderEngine::build_view(const Window& w) const {
    auto view = build_view_without_working(w);
    if (tui_.router_->busy() && working_visible_) {
        if (!view.empty() && !view.back().runs.empty())
            view.push_back(rich::Line{});
        auto now = std::chrono::steady_clock::now();
        size_t secs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - working_since_).count());
        std::string label = tool_display::working_label(
            text::glyph::spinner_round(anim_phase_), secs, tui_.running_tool_desc_);
        rich::Line wl;
        rich::Run r;
        r.pair = P_STATUS;
        r.text = label;
        wl.runs.push_back(std::move(r));
        view.push_back(std::move(wl));
        view.push_back(rich::Line{});
    }
    return view;
}

int RenderEngine::max_scroll(const Window& w) const {
    auto view = build_view_without_working(w);
    int total = static_cast<int>(rich::rewrap_all(view, width()).size());
    bool show_working = tui_.router_->busy() && working_visible_;
    int ch = chat_height();
    if (show_working) ch = std::max(1, ch - 1);
    int m = total - ch;
    return m < 0 ? 0 : m;
}

int RenderEngine::max_scroll() const { return max_scroll(tui_.win()); }

size_t RenderEngine::utf8_len(const std::string& s, size_t i) {
    return text::utf8_len(s, i);
}
std::vector<std::string> RenderEngine::wrap_text(const std::string& text, int w) {
    return text::wrap(text, w);
}
void RenderEngine::append_rich_to(std::vector<rich::Line>& view, const std::string& text,
                                  int color, int w) {
    rich::Line l;
    rich::Run r; r.pair = color; r.text = text;
    l.runs.push_back(r);
    for (auto& x : rich::wrap(l, w)) view.push_back(std::move(x));
}
void RenderEngine::append_rich_to(Window& w, const rich::Line& l) {
    tui_.append_rich_to(w, l);
}

int RenderEngine::display_cols(const std::string& s) { return text::display_cols(s); }
std::wstring RenderEngine::to_wide(const std::string& s) { return text::to_wide(s); }
std::string RenderEngine::kfmt(long n) { return agent::bar::kfmt(n); }

int RenderEngine::gauge_pair(double f) {
    switch (agent::bar::pressure(f)) {
        case agent::bar::Pressure::Crit: return P_GAUGE_CRIT;
        case agent::bar::Pressure::Warn: return P_GAUGE_WARN;
        default:                         return P_GAUGE_OK;
    }
}

std::vector<RenderEngine::Seg> RenderEngine::bar_segments() const {
    std::vector<Seg> segs;
    std::string wtag = "[" + std::to_string(tui_.window_manager_->active() + 1) + "/" +
                       std::to_string(tui_.window_manager_->count()) + "]";
    segs.push_back({wtag, P_BANNER, 3});
    segs.push_back({" [" + tui_.cfg_.model +
                        tool_display::reasoning_badge(tui_.cfg_.reasoning_effort) +
                        "]",
                    P_GAUGE_OK, 5});

    std::string mode_txt;
    int mode_pair = P_BAR_DIM;
    switch (tui_.cfg_.mode) {
        case agent::AgentMode::Read:
            mode_txt = " read ";
            mode_pair = P_GAUGE_OK;
            break;
        case agent::AgentMode::Write:
            mode_txt = " write ";
            mode_pair = P_GAUGE_WARN;
            break;
        case agent::AgentMode::Yolo:
            mode_txt = " yolo ";
            mode_pair = P_BUTTON_ACT;
            break;
    }
    segs.push_back({mode_txt, mode_pair, 2});

    if (scroll_mode_) {
        segs.push_back({" S ", P_GAUGE_OK, 0});
    }

    if (tui_.stats_.latency_ms >= 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  lag %.0fms", tui_.stats_.latency_ms);
        int lag_pair;
        if (tui_.stats_.latency_ms > 5000) lag_pair = P_GAUGE_CRIT;
        else if (tui_.stats_.latency_ms > 1000) lag_pair = P_GAUGE_WARN;
        else lag_pair = P_BAR_DIM;
        segs.push_back({b, lag_pair, 6});
    } else {
        segs.push_back({"  lag " + std::string(text::glyph::emdash()), P_BAR_DIM, 6});
    }
    if (tui_.stats_.tps > 0) {
        char b[32];
        std::snprintf(b, sizeof(b), "  %.0f t/s", tui_.stats_.tps);
        segs.push_back({b, P_BAR_DIM, 4});
    } else {
        segs.push_back({"  " + std::string(text::glyph::emdash()) + " t/s", P_BAR_DIM, 4});
    }
    std::string up = tui_.stats_.prompt_tokens >= 0 ? kfmt(tui_.stats_.prompt_tokens)
                                                    : text::glyph::emdash();
    std::string dn = tui_.stats_.completion_tokens >= 0
                         ? kfmt(tui_.stats_.completion_tokens) : text::glyph::emdash();
    segs.push_back({"  " + std::string(text::glyph::up()) + up + " " +
                    text::glyph::down() + dn, P_BAR_DIM, 7});

    int njobs = tui_.jobs_.running_count();
    if (njobs > 0) {
        std::string s = "  " + std::to_string(njobs) + " job" +
                        (njobs > 1 ? "s" : "");
        int rem = tui_.jobs_.min_timeout_remaining();
        if (rem >= 0) s += " " + std::to_string(rem) + "s";
        segs.push_back({s, P_GAUGE_WARN, 1});
    } else if (!tui_.running_tool_.empty()) {
        segs.push_back({"  " + tui_.running_tool_ + "…", P_GAUGE_WARN, 1});
    }

    std::string mcp_txt;
    for (const auto& st : tui_.mcp_servers_.snapshot()) {
        if (!st.connected && st.error.empty()) continue;
        if (!mcp_txt.empty()) mcp_txt += "·";
        mcp_txt += (st.connected ? "" : "!") + st.name;
    }
    if (!mcp_txt.empty())
        segs.push_back({"  mcp: " + mcp_txt, P_BAR_DIM, 8});
    return segs;
}

void RenderEngine::draw() {
    dirty_ = true;
    if (tui_.win().welcome_art) {
        erase();
        welcome::render(stdscr, chat_top(), width());
        draw_status_bar("welcome");
        wnoutrefresh(stdscr);
        return;
    }

    bool show_working = tui_.router_->busy() && working_visible_;
    std::vector<rich::Line> view = build_view_without_working(tui_.win());
    int ch = chat_height();
    if (show_working) ch = std::max(1, ch - 1);
    chat_canvas_.resize(chat_top(), ch, width());
    chat_canvas_.set_lines(view);
    if (static_cast<size_t>(tui_.win().scroll_top) >
        static_cast<size_t>(chat_canvas_.max_top()))
        tui_.win().scroll_top = chat_canvas_.max_top();
    chat_canvas_.set_top(tui_.win().scroll_top);
    chat_canvas_.render();
    if (show_working) {
        int wy = chat_top() + ch;
        move(wy, 0);
        clrtoeol();
        auto now = std::chrono::steady_clock::now();
        size_t secs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - working_since_).count());
        std::string label = tool_display::working_label(
            text::glyph::spinner_round(anim_phase_), secs, tui_.running_tool_desc_);
        attron(COLOR_PAIR(P_STATUS));
        mvaddnstr(wy, 0, label.c_str(), width());
        attroff(COLOR_PAIR(P_STATUS));
    }

    {
        int total = chat_canvas_.wrapped_count();
        if (show_working) total += 1;
        int pos = tui_.win().scroll_top;
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

void RenderEngine::draw_status_bar(const std::string& tail) {
    int w = width();
    int y = height() - 2;

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char clk[16];
    std::strftime(clk, sizeof(clk), "[%H:%M:%S]", &tm);
    std::string clock = clk;
    int clock_w = display_cols(clock);

    constexpr int kIW = 12;
    int activity_w = kIW + 1;

    attron(COLOR_PAIR(P_BANNER));
    mvhline(y, 0, ' ', w);
    attroff(COLOR_PAIR(P_BANNER));

    std::vector<Seg> segs = bar_segments();

    bool have_ctx = (tui_.cfg_.context_size > 0);
    long ctx_used_base = tui_.ctx_used_.load();
    long ctx_used = ctx_used_base >= 0 ? ctx_used_base + tui_.live_ctx_offset_
                                       : tui_.live_ctx_offset_;
    double frac = have_ctx
                      ? static_cast<double>(ctx_used) / tui_.cfg_.context_size
                      : 0.0;

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
                          kfmt(tui_.cfg_.context_size).c_str());
            put(b, gauge_pair(frac));
        }
    }

    if (!tail.empty() && x + display_cols(tail) + 1 < budget)
        put("  " + tail, P_BAR_DIM);

    int ix = w - clock_w - kIW - 1;
    if (ix > x + 4) {
        wattron(stdscr, COLOR_PAIR(P_BAR_DIM));
        mvaddch(y, ix, '[');
        if (tui_.router_->busy()) {
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
            mvaddstr(y, ix + 1, "   idle   ");
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

void RenderEngine::tick_clock() {
    dirty_ = true;
    int total = max_scroll() + chat_height();
    int vis = chat_height();
    int pos = tui_.win().scroll_top;
    std::string tail;
    if (total > vis) {
        int pct = 100 - static_cast<int>(100.0 * std::min(pos, total - vis)
                                         / (total - vis));
        tail = " P:" + std::to_string(pct) + "%";
    }
    draw_status_bar(tail);
    wnoutrefresh(stdscr);
}

void RenderEngine::draw_input(const std::string& s, size_t cursor, const std::string& shadow) {
    dirty_ = true;
    draw_drawer(s);
    int y = height() - 1;
    int w = width();
    int x = 0;
    int prompt_w = 0;

    move(y, 0);
    clrtoeol();

    auto put = [&](const std::string& text, int pair, int attrs = 0) {
        if (x >= w) return;
        std::wstring ws = to_wide(text);
        int room = w - x;
        if (static_cast<int>(ws.size()) > room) ws.resize(room);
        if (attrs)
            attron(COLOR_PAIR(pair) | attrs);
        else
            attron(COLOR_PAIR(pair));
        mvaddnwstr(y, x, ws.c_str(), static_cast<int>(ws.size()));
        if (attrs)
            attroff(COLOR_PAIR(pair) | attrs);
        else
            attroff(COLOR_PAIR(pair));
        x += static_cast<int>(ws.size());
    };

    auto decor = [&](const std::string& t) { put(t, P_USER, A_DIM); };
    decor("\u2514\u2500[");
    put(git_project_, P_USER);
    if (!git_branch_.empty()) {
        decor("]\u2500[");
        put(git_branch_, P_ASSISTANT);
        if (git_ins_ > 0 || git_del_ > 0) {
            decor("]\u2500[");
            if (git_ins_ > 0)
                put("+" + std::to_string(git_ins_), P_GIT_PLUS);
            decor("/");
            if (git_del_ > 0)
                put("-" + std::to_string(git_del_), P_GIT_MINUS);
        }
    }
    decor("]\u2500\u276f ");

    prompt_w = x;
    int total_w = prompt_w + display_cols(s) + display_cols(shadow);
    int cursor_col = prompt_w + display_cols(s.substr(0, cursor));
    int scroll_off = 0;
    if (cursor_col >= w) scroll_off = cursor_col - w + 1;
    if (scroll_off < prompt_w) scroll_off = 0;
    if (total_w - scroll_off <= 0) {
        scroll_off = std::max(0, total_w - w);
    }

    int input_start = prompt_w - scroll_off;
    if (input_start < 0) input_start = 0;
    if (input_start < w && scroll_off > prompt_w) {
        const char* input_visible = s.c_str();
        auto skip = static_cast<size_t>(scroll_off - prompt_w);
        int input_len;
        if (skip < s.size()) {
            input_visible += skip;
            input_len = static_cast<int>(s.size()) - static_cast<int>(skip);
        } else {
            input_len = 0;
        }
        if (input_len > 0) {
            if (input_len > w) input_len = w;
            attron(COLOR_PAIR(P_USER));
            mvaddnstr(y, input_start, input_visible, input_len);
            attroff(COLOR_PAIR(P_USER));
        }
    } else if (input_start < w && scroll_off <= prompt_w) {
        const char* input_visible = s.c_str();
        int input_len = static_cast<int>(s.size());
        if (input_len > w - input_start)
            input_len = w - input_start;
        if (input_len > 0) {
            attron(COLOR_PAIR(P_USER));
            mvaddnstr(y, input_start, input_visible, input_len);
            attroff(COLOR_PAIR(P_USER));
        }
    }

    if (!shadow.empty() && cursor == s.size()) {
        int input_w = prompt_w + display_cols(s);
        int shadow_start = input_w - scroll_off;
        if (shadow_start >= 0 && shadow_start < w) {
            attron(A_DIM | COLOR_PAIR(P_INPUT_SHADOW));
            mvaddnstr(y, shadow_start, shadow.c_str(),
                       std::min(static_cast<int>(shadow.size()), w - shadow_start));
            attroff(A_DIM | COLOR_PAIR(P_INPUT_SHADOW));
        }
    }

    int cx = cursor_col - scroll_off;
    if (cx < 0) cx = 0;
    if (cx >= w) cx = w - 1;
    curs_set(1);
    move(y, cx);
    wnoutrefresh(stdscr);
}





void RenderEngine::draw_drawer(const std::string& input) {
    if (!drawer_open_) return;
    if (tui_.modal_open_) {
        int bar_row = height() - 2;
        for (int row = std::max(0, bar_row - 8); row < bar_row; ++row) {
            move(row, 0); clrtoeol();
        }
        return;
    }

    int bar_row = height() - 2;
    bool arg_mode = drawer_has_arg(input);
    std::vector<std::string> rows = drawer_rows(input, tui_.settings_);

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

void RenderEngine::git_refresh() {
    auto read_stdout = [](const char* cmd) -> std::string {
        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return result;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe))
            result += buf;
        pclose(pipe);
        return result;
    };

    {
        std::string cwd;
        std::array<char, 4096> buf;
        if (getcwd(buf.data(), buf.size()))
            cwd = buf.data();
        size_t slash = cwd.rfind('/');
        git_project_ = (slash == std::string::npos) ? cwd : cwd.substr(slash + 1);
        if (git_project_.empty()) git_project_ = "project";
    }

    std::string ref = read_stdout("git symbolic-ref HEAD 2>/dev/null");
    if (ref.empty()) {
        git_branch_.clear();
        git_ins_ = 0;
        git_del_ = 0;
        return;
    }
    ref.erase(ref.find_last_not_of(" \n\r") + 1);
    if (ref.compare(0, 11, "refs/heads/") == 0)
        git_branch_ = ref.substr(11);
    else
        git_branch_ = ref;

    std::string stat = read_stdout(
        "git diff --shortstat -- . ':!bench/results' ':!*.o' ':!*.a' ':!*.d' 2>/dev/null");
    git_ins_ = 0;
    git_del_ = 0;
    if (!stat.empty()) {
        auto extract = [&](const std::string& needle) -> int {
            size_t pos = stat.find(needle);
            if (pos == std::string::npos) return 0;
            size_t start = pos;
            while (start > 0 && isdigit(static_cast<unsigned char>(stat[start - 1])))
                --start;
            return std::stoi(stat.substr(start, pos - start));
        };
        git_ins_ = extract(" insertion");
        git_del_ = extract(" deletion");
    }
}

} // namespace tui