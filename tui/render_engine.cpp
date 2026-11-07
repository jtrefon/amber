#include "render_engine.h"
#include "drawer_view.h"
#include "git_reads.h"
#include "menu_controller.h"
#include "top_bar.h"
#include "tui.h"
#include "welcome.h"
#include "tool_display.h"
#include "task_runtime.h"

#include "agent/compact.h"
#include "agent/task.h"
#include "tui/drawer_rows.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unistd.h>

namespace tui {

RenderEngine::RenderEngine(Tui& tui) : tui_(tui), pulse_epoch_(std::chrono::steady_clock::now()) {
    // Seed the project name (getcwd, no fork) so the prompt is complete on the
    // first paint; branch and diff counts arrive from the worker.
    auto seed = std::make_shared<GitState>();
    seed->project = current_project_name();
    std::atomic_store_explicit(&git_pub_->state, std::shared_ptr<const GitState>(std::move(seed)),
                               std::memory_order_release);
}

std::string RenderEngine::current_project_name(const std::string& root) {
    std::string dir = root;
    if (dir.empty()) {
        std::array<char, 4096> buf;
        if (getcwd(buf.data(), buf.size()))
            dir = buf.data();
    }
    size_t slash = dir.rfind('/');
    std::string project = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
    return project.empty() ? std::string("project") : project;
}

void RenderEngine::mark_working() noexcept {
    working_since_ = std::chrono::steady_clock::now();
    working_visible_ = true;
}

int RenderEngine::height() const {
    int y, x;
    getmaxyx(stdscr, y, x);
    (void)x;
    return y;
}
int RenderEngine::width() const {
    int y, x;
    getmaxyx(stdscr, y, x);
    (void)y;
    return x;
}
int RenderEngine::top_rows() const {
    // One row: the rail carries the mark, the tabs and the clock. The second row
    // that underlined the active tab cost a line of transcript to say what the
    // reverse-video block already says.
    return height() >= 4 ? 1 : 0;
}
std::optional<std::size_t> RenderEngine::tab_at(int x, int y) const {
    if (y != 0 || top_rows() <= 0)
        return std::nullopt;
    for (const auto& hit : top_layout_.hits)
        if (x >= hit.start && x < hit.start + hit.width)
            return hit.id;
    return std::nullopt;
}

std::optional<std::size_t> RenderEngine::tab_close_at(int x, int y) const {
    if (y != 0 || top_rows() <= 0)
        return std::nullopt;
    for (const auto& hit : top_layout_.hits)
        if (x == hit.close_start)
            return hit.id;
    return std::nullopt;
}

std::optional<menu::Hit> RenderEngine::menu_hit(int x, int y) const {
    return menu::hit_test(menu_layout_, x, y);
}

int RenderEngine::chat_top() const {
    return top_rows();
}
int RenderEngine::chat_height() const {
    return std::max(1, height() - top_rows() - 2);
}
int RenderEngine::lines_per_page() const {
    return chat_height();
}

// The live thinking block, when it is shown.
void RenderEngine::append_reasoning(std::vector<rich::Line>& view, const Window& w) const {
    if (!show_reasoning_ || !w.reason.active())
        return;
    rich::Line label;
    rich::Run r0;
    r0.pair = P_REASONING;
    r0.dim = true;
    r0.text = "thinking...";
    label.runs.push_back(r0);
    view.push_back(label);

    rich::Line body;
    rich::Run r1;
    r1.pair = P_REASONING;
    r1.dim = true;
    r1.text = w.reason.buffer;
    body.runs.push_back(r1);
    for (auto& l : rich::wrap(body, width()))
        view.push_back(std::move(l));
    if (!w.stream_buf.empty())
        view.push_back(rich::Line{});
}

// The in-flight assistant text: a markdown preview, or plain styled text.
void RenderEngine::append_stream_preview(std::vector<rich::Line>& view, const Window& w) const {
    if (w.stream_buf.empty())
        return;
    if (!w.markdown_on) {
        append_rich_to(view, w.stream_buf, w.stream_color, width());
        return;
    }
    auto preview = md::render(w.stream_buf, md_style_);
    if (preview.empty())
        return;
    rich::Run ts;
    ts.text = (w.stream_ts.empty() ? Tui::timestamp() : w.stream_ts) + " ";
    ts.pair = P_REASONING;
    ts.dim = true;
    preview.front().runs.insert(preview.front().runs.begin(), std::move(ts));
    for (auto& l : preview)
        view.push_back(std::move(l));
}

namespace {

// A blank line before and after every rule, so it is never glued to text.
std::vector<rich::Line> space_rules(const std::vector<rich::Line>& view) {
    std::vector<rich::Line> fixed;
    fixed.reserve(view.size() + 4);
    for (size_t i = 0; i < view.size(); ++i) {
        if (!view[i].is_hr) {
            fixed.push_back(view[i]);
            continue;
        }
        if (!fixed.empty() && !fixed.back().runs.empty())
            fixed.push_back(rich::Line{});
        fixed.push_back(view[i]);
        const bool next_is_blank = (i + 1 < view.size() && view[i + 1].runs.empty());
        if (!next_is_blank)
            fixed.push_back(rich::Line{});
    }
    return fixed;
}

} // namespace

std::vector<rich::Line> RenderEngine::build_view_without_working(const Window& w) const {
    std::vector<rich::Line> view = w.lines;
    append_reasoning(view, w);
    append_stream_preview(view, w);
    const bool live = tui_.runs_.busy(w.id) && (!w.stream_buf.empty() || !w.reason.buffer.empty());
    if (live && (view.empty() || !view.back().runs.empty()))
        view.push_back(rich::Line{});
    return space_rules(view);
}

// The word leading the working indicator. Priority and wording live in the
// pure tool_display::activity_verb: an active tool wins over the run-state
// word, so "waiting" only appears when nothing is in flight. While a batch
// of tool results closes out of order, fall back to the next queued call.
std::string RenderEngine::activity_verb() const {
    // The verb describes the ACTIVE window's run — sibling agents keep
    // their own state on their own Window.
    const Window& w = tui_.win();
    std::string tool = w.running_tool;
    if (tool.empty()) {
        const auto& pending = tui_.router_->pending_tools();
        for (const auto& pt : pending)
            if (pt.window_id == w.id) {
                tool = pt.name;
                break;
            }
    }
    return tool_display::activity_verb(w.compressing, w.state, tool, tui_.reg_);
}

int RenderEngine::max_scroll(const Window& w) const {
    auto view = build_view_without_working(w);
    int total = static_cast<int>(rich::rewrap_all(view, width()).size());
    bool show_working = tui_.runs_.busy(w.id) && working_visible_;
    int ch = chat_height();
    if (show_working)
        ch = std::max(1, ch - 1);
    int m = total - ch;
    return m < 0 ? 0 : m;
}

int RenderEngine::max_scroll() const {
    return max_scroll(tui_.win());
}

size_t RenderEngine::utf8_len(const std::string& s, size_t i) {
    return text::utf8_len(s, i);
}
std::vector<std::string> RenderEngine::wrap_text(const std::string& text, int w) {
    return text::wrap(text, w);
}
void RenderEngine::append_rich_to(std::vector<rich::Line>& view, const std::string& text, int color,
                                  int w) {
    rich::Line l;
    rich::Run r;
    r.pair = color;
    r.text = text;
    l.runs.push_back(r);
    for (auto& x : rich::wrap(l, w))
        view.push_back(std::move(x));
}
void RenderEngine::append_rich_to(Window& w, const rich::Line& l) {
    tui_.append_rich_to(w, l);
}

int RenderEngine::display_cols(const std::string& s) {
    return text::display_cols(s);
}
std::wstring RenderEngine::to_wide(const std::string& s) {
    return text::to_wide(s);
}

int RenderEngine::gauge_pair(double f) {
    switch (agent::bar::pressure(f)) {
    case agent::bar::Pressure::Crit:
        return P_GAUGE_CRIT;
    case agent::bar::Pressure::Warn:
        return P_GAUGE_WARN;
    default:
        return P_GAUGE_OK;
    }
}

// Publish what the bar may show. Segments (core and plugin alike) are pure
// functions of this snapshot, so nothing on the bar reaches into TUI state.
agent::StatusSnapshot RenderEngine::build_status_snapshot() const {
    agent::StatusSnapshot snapshot;
    snapshot.window_index = static_cast<int>(tui_.window_manager_->chat_position());
    snapshot.window_count = static_cast<int>(tui_.window_manager_->chat_count());
    // The bar describes the ACTIVE window: model/mode/telemetry are that
    // window's agent state, not process globals.
    const Window& w = tui_.win();
    // One immutable snapshot for the whole bar: the agent's worker may be
    // running, and cfg_ is its private working copy.
    const auto acfg = w.agent ? w.agent->config_snapshot() : nullptr;
    snapshot.model = acfg ? acfg->model : tui_.cfg_.model;
    // The provider qualifies the model, and a vendor-prefixed id is not the
    // provider: "kilo-auto/free" is kilo's model, served by kilocode.
    snapshot.provider = acfg ? acfg->provider_name : tui_.cfg_.provider_name;
    snapshot.reasoning_effort = acfg ? acfg->reasoning_effort : tui_.cfg_.reasoning_effort;
    snapshot.mode = acfg ? acfg->mode : tui_.cfg_.mode;
    snapshot.scroll_mode = scroll_mode_;
    // How far above the live edge the viewport is: -1 when everything fits, 0
    // at the edge, 1..100 up from it. scroll_top counts from the top of the
    // buffer, so the live edge is max_top (see scroll_pct).
    snapshot.scroll_pct = status_bar_layout::scroll_pct(w.scroll_top, chat_canvas_.max_top());
    snapshot.latency_ms = static_cast<long>(w.stats.latency_ms);
    snapshot.tps = w.stats.tps;
    snapshot.usage = w.usage;
    snapshot.running_tasks = 0;
    snapshot.undelivered_tasks = 0;
    if (tui_.tasks_) {
        for (const auto& record : tui_.tasks_->list(w.owner_id)) {
            if (!agent::task_state_terminal(record.state))
                ++snapshot.running_tasks;
        }
        snapshot.undelivered_tasks =
            static_cast<int>(tui_.task_runtime_->inbox.pending(w.owner_id));
    }
    if (tui_.reg_.find("task")) {
        snapshot.subagents_available = true;
        snapshot.active_agents = tui_.subagents_.active();
        snapshot.waiting_agents = tui_.subagents_.waiting();
        snapshot.max_agents = tui_.subagents_.max();
    }
    snapshot.running_tool = w.running_tool;
    return snapshot;
}

// Semantic tone -> the terminal's palette. The registry speaks tones so a
// plugin never names a colour pair.
int RenderEngine::tone_pair(agent::StatusTone tone) {
    switch (tone) {
    case agent::StatusTone::Good:
        return P_GAUGE_OK;
    case agent::StatusTone::Warn:
        return P_GAUGE_WARN;
    case agent::StatusTone::Crit:
        return P_GAUGE_CRIT;
    case agent::StatusTone::Accent:
        return P_BUTTON_ACT;
    case agent::StatusTone::Banner:
        return P_BANNER;
    case agent::StatusTone::Dim:
        break;
    }
    return P_BAR_DIM;
}

std::vector<RenderEngine::Seg> RenderEngine::bar_segments() const {
    std::vector<Seg> segs;
    for (auto& segment : tui_.plugin_runtime_.status().render(build_status_snapshot())) {
        if (segment.zone != agent::StatusZone::Bottom)
            continue;
        segs.push_back({std::move(segment.text), tone_pair(segment.tone), segment.drop_priority,
                        segment.align, segment.id});
    }
    return segs;
}

std::vector<RenderEngine::Seg> RenderEngine::top_segments() const {
    std::vector<Seg> segs;
    for (auto& segment : tui_.plugin_runtime_.status().render(build_status_snapshot())) {
        if (segment.zone != agent::StatusZone::Top || segment.align != agent::StatusAlign::Right)
            continue;
        segs.push_back({std::move(segment.text), tone_pair(segment.tone), segment.drop_priority,
                        segment.align, segment.id});
    }
    return segs;
}

void RenderEngine::draw_working_indicator(int row) {
    move(row, 0);
    clrtoeol();
    const auto now = std::chrono::steady_clock::now();
    const size_t secs = static_cast<size_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - working_since_).count());
    const auto& win = tui_.win();
    const tool_display::WorkingParts parts =
        tool_display::working_parts(text::glyph::spinner_round(anim_phase_), activity_verb(), secs,
                                    win.running_tool_desc, win.running_intent);
    std::string tail = parts.tail;
    const std::string stuck = tool_display::stuck_hint(secs);
    if (!stuck.empty())
        tail += " \u00b7 " + stuck;
    // The glyph steps and the verb carries the sweep: the left side has a coarse
    // motion and a fine one at the same time. Wide strings are painted by
    // display column, so a multibyte glyph is never split (mvaddnstr counts
    // BYTES and used to leave a mojibake fragment).
    const int w = width();
    int x = 0;
    put_wsegment(row, w, x, to_wide(parts.glyph + " "), P_STATUS);
    draw_shine(row, w, x, to_wide(parts.verb), P_STATUS, {0, agent::bar::ShineStyle::Gradient});
    put_wsegment(row, w, x, to_wide(tail), P_STATUS);
}

// The scroll indicator used to live here as a " P:0%" tail appended after the
// gauge. It is a core segment now (snapshot.scroll_pct), so it participates in
// the layout's width arithmetic like every other readout.

void RenderEngine::draw() {
    dirty_ = true;
    tick_anim();
    working_row_owned_ = false;
    if (tui_.win().welcome_art) {
        erase();
        draw_top_bar();
        welcome::render(stdscr, chat_top(), width());
        draw_menu_dropdown();
        draw_status_bar();
        wnoutrefresh(stdscr);
        return;
    }

    draw_top_bar();
    const bool show_working = tui_.runs_.busy(tui_.win().id) && working_visible_;
    // The canvas gives up a row only when this is true; tick_pulse repaints that
    // row without resizing the canvas, so it must not touch a row the transcript
    // still owns.
    working_row_owned_ = show_working;
    std::vector<rich::Line> view = build_view_without_working(tui_.win());
    int ch = chat_height();
    if (show_working)
        ch = std::max(1, ch - 1);
    chat_canvas_.resize(chat_top(), ch, width());
    chat_canvas_.set_lines(view);
    if (static_cast<size_t>(tui_.win().scroll_top) > static_cast<size_t>(chat_canvas_.max_top()))
        tui_.win().scroll_top = chat_canvas_.max_top();
    chat_canvas_.set_top(tui_.win().scroll_top);
    chat_canvas_.render();
    if (show_working)
        draw_working_indicator(working_row());
    draw_menu_dropdown();

    draw_status_bar();
    wnoutrefresh(stdscr);
}

namespace {

// The sweep's period and the frame the spin glyph steps through. Both read the
// wall clock: the paint cadence can change without changing how fast the pulse
// travels, and a skipped paint cannot make it drift.
constexpr double kPulsePeriodMs = 1800.0;
constexpr int kFrameMs = 150;

// The gauge's band is a glint, not a wash: three lit cells in the single light
// shade, whatever the gauge is wide. The verb keeps the proportional band with
// its fade, which is what makes a word read as a sweep rather than a dot.
constexpr int kGaugeShineCells = 3;

} // namespace

// Paint `s` at x within the budget, clipping by display columns; advances x.
void RenderEngine::put_segment(int y, int budget, int& x, const std::string& s, int pair) {
    put_wsegment(y, budget, x, to_wide(s), pair);
}

void RenderEngine::put_wsegment(int y, int budget, int& x, const std::wstring& ws, int pair) {
    if (x >= budget)
        return;
    const auto room = static_cast<size_t>(budget - x);
    const size_t n = std::min(ws.size(), room);
    if (n == 0)
        return;
    attron(COLOR_PAIR(pair));
    mvaddnwstr(y, x, ws.c_str(), static_cast<int>(n));
    attroff(COLOR_PAIR(pair));
    x += static_cast<int>(n);
}

void RenderEngine::put_span(int y, int w, int& x, const std::string& text, int pair, int attrs) {
    if (x >= w || text.empty())
        return;
    std::wstring ws = to_wide(text);
    const auto room = static_cast<size_t>(w - x);
    if (ws.size() > room)
        ws.resize(room);
    if (ws.empty())
        return;
    attron(COLOR_PAIR(pair) | attrs);
    mvaddnwstr(y, x, ws.c_str(), static_cast<int>(ws.size()));
    attroff(COLOR_PAIR(pair) | attrs);
    x += static_cast<int>(ws.size());
}

int RenderEngine::shine_pair(int base_pair, int level) {
    if (level <= 0)
        return base_pair;
    // The gauge runs a solid band, so its lit cells are always the core shade;
    // only the verb asks for the fade step.
    struct Shade {
        int base, mid, hi;
    };
    static constexpr Shade kShades[] = {
        {P_GAUGE_OK, P_GAUGE_OK_HI, P_GAUGE_OK_HI},
        {P_GAUGE_WARN, P_GAUGE_WARN_HI, P_GAUGE_WARN_HI},
        {P_GAUGE_CRIT, P_GAUGE_CRIT_HI, P_GAUGE_CRIT_HI},
        {P_STATUS, P_TEXT_SHINE_MID, P_TEXT_SHINE_HI},
    };
    for (const Shade& s : kShades) {
        if (s.base == base_pair)
            return level == 1 ? s.mid : s.hi;
    }
    return base_pair;
}

// Paint a run of one-column cells with the moving band shaded over it while a
// run is live; at rest the whole run keeps the base shade, so an idle bar does
// not appear to be working. Cells are wide-string indices: every glyph the gauge
// and the verb use is one column, which is what makes the two the same
// coordinate. `band` is the band's width in cells (0 = a third of the run);
// `style` decides whether the band fades or is one solid shade.
void RenderEngine::draw_shine(int y, int budget, int& x, const std::wstring& cells, int base_pair,
                              Shine shine) {
    if (cells.empty())
        return;
    if (!tui_.runs_.busy(tui_.win().id)) {
        put_wsegment(y, budget, x, cells, base_pair);
        return;
    }
    const int n = static_cast<int>(cells.size());
    const double phase = anim_phase01();
    for (int i = 0; i < n;) {
        const int level = agent::bar::shine_level(i, n, phase, shine.band, shine.style);
        int end = i + 1;
        while (end < n && agent::bar::shine_level(end, n, phase, shine.band, shine.style) == level)
            ++end;
        put_wsegment(y, budget, x,
                     cells.substr(static_cast<size_t>(i), static_cast<size_t>(end - i)),
                     shine_pair(base_pair, level));
        i = end;
    }
}

int RenderEngine::context_window() const {
    const Window& w = tui_.win();
    if (w.agent) {
        const auto acfg = w.agent->config_snapshot();
        if (acfg)
            return acfg->context_size;
    }
    return tui_.cfg_.context_size;
}

// The context gauge: a labelled bar when the window is known, the live count
// alone otherwise.
void RenderEngine::draw_ctx_gauge(int y, int budget, int& x, bool have_ctx, long ctx_used,
                                  double frac) {
    if (x >= budget)
        return;
    put_segment(y, budget, x, "  ctx ", P_BAR_DIM);
    if (!have_ctx) {
        // Unknown is a state, not an empty slot: show the live count when there
        // is one, and a dash otherwise. Returning here used to make the whole
        // gauge vanish at 0/auto.
        if (ctx_used > 0)
            put_segment(y, budget, x, agent::compact::count(ctx_used), P_BAR_DIM);
        else
            put_segment(y, budget, x, text::glyph::utf8() ? "\u2014" : "?", P_BAR_DIM);
        return;
    }
    const int cells = std::min(24, std::max(6, (budget - x) - 14));
    if (cells <= 0 || x >= budget)
        return;
    put_segment(y, budget, x, text::glyph::block_l(), P_BAR_DIM);
    const std::string bar = text::glyph::utf8() ? agent::bar::gauge_bar(frac, cells)
                                                : agent::bar::gauge_bar_ascii(frac, cells);
    draw_shine(y, budget, x, to_wide(bar), gauge_pair(frac),
               {kGaugeShineCells, agent::bar::ShineStyle::Solid});
    put_segment(y, budget, x, text::glyph::block_r(), P_BAR_DIM);
    const std::string readout =
        " " + agent::compact::percent(static_cast<int>(std::lround(frac * 100))) + "% " +
        agent::compact::count(ctx_used) + "/" + agent::compact::count(context_window());
    put_segment(y, budget, x, readout, gauge_pair(frac));
}

int RenderEngine::working_row() const {
    return chat_top() + std::max(1, chat_height() - 1);
}

// The frame the spin glyph steps through. The sweep is not driven from here: it
// reads the wall clock, so a repaint that arrives early or late cannot change
// how fast it travels.
void RenderEngine::tick_anim() {
    if (!tui_.runs_.busy(tui_.win().id)) {
        anim_phase_ = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (anim_tick_.time_since_epoch().count() == 0 ||
        now - anim_tick_ > std::chrono::milliseconds(kFrameMs)) {
        ++anim_phase_;
        anim_tick_ = now;
    }
}

double RenderEngine::anim_phase01() const {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - pulse_epoch_)
                        .count();
    return std::fmod(static_cast<double>(ms) / kPulsePeriodMs, 1.0);
}

// The two rows that animate while a run is live, and only those: the transcript
// keeps its own, slower cadence.
void RenderEngine::tick_pulse() {
    tick_anim();
    if (working_row_owned_ && tui_.runs_.busy(tui_.win().id))
        draw_working_indicator(working_row());
    draw_status_bar();
    wnoutrefresh(stdscr);
    dirty_ = true;
}

// The right zone, laid out left to right so the highest priority (the last
// entry) ends up against the edge.
void RenderEngine::draw_right_zone(int y, int w, int right_w, const std::vector<Seg>& right_zone) {
    int rx = w - right_w;
    for (size_t i = 0; i < right_zone.size(); ++i) {
        if (i)
            ++rx;
        std::wstring text = to_wide(right_zone[i].text);
        if (rx + static_cast<int>(text.size()) > w)
            text.resize(static_cast<size_t>(std::max(0, w - rx)));
        attron(COLOR_PAIR(right_zone[i].pair));
        mvaddnwstr(y, rx, text.c_str(), static_cast<int>(text.size()));
        attroff(COLOR_PAIR(right_zone[i].pair));
        rx += static_cast<int>(text.size());
    }
}

void RenderEngine::draw_status_bar() {
    const int w = width();
    const int y = height() - 2;

    // The clock used to be built here and its width reserved before the bar
    // knew what else it held. It is a right-aligned segment now (the clock
    // plugin), so this function lays out registered segments and owns no
    // readout of its own: it does not know a clock exists.
    attron(COLOR_PAIR(P_BANNER));
    mvhline(y, 0, ' ', w);
    attroff(COLOR_PAIR(P_BANNER));

    const int window = context_window();
    const bool have_ctx = (window > 0);
    const Window& aw = tui_.win();
    const long ctx_used =
        tool_display::gauge_tokens(aw.ctx_used.load(), aw.ctx_estimate.load(), aw.live_ctx_offset);
    const double frac = have_ctx ? static_cast<double>(ctx_used) / window : 0.0;

    // Width arbitration is pure (status_bar_layout); this function only paints
    // the plan it returns.
    const status_bar_layout::Plan plan = status_bar_layout::plan(bar_segments(), w);
    const int budget = plan.budget;

    int x = 0;
    for (const auto& s : plan.left)
        put_segment(y, budget, x, s.text, s.pair);

    draw_ctx_gauge(y, budget, x, have_ctx, ctx_used, frac);

    if (plan.right_cols > 0)
        draw_right_zone(y, w, plan.right_cols, plan.right);
}

namespace {

void top_role_style(top_bar::Role role, int& pair, int& attrs) {
    pair = P_BANNER;
    attrs = 0;
    switch (role) {
    case top_bar::Role::AppMark:
        pair = P_BRAND;
        attrs = A_BOLD;
        break;
    case top_bar::Role::ActiveTab:
        attrs = A_REVERSE | A_BOLD;
        break;
    case top_bar::Role::InactiveTab:
        // The clock's own pair: dim-on-banner melted into the bar.
        pair = P_BAR_DIM;
        break;
    case top_bar::Role::RunningTab:
    case top_bar::Role::DoneTab:
        pair = P_GAUGE_OK;
        attrs = A_BOLD;
        break;
    case top_bar::Role::AttentionTab:
        pair = P_BUTTON_ACT;
        attrs = A_BOLD;
        break;
    case top_bar::Role::ErrorTab:
        pair = P_GAUGE_CRIT;
        attrs = A_BOLD;
        break;
    case top_bar::Role::Overflow:
        pair = P_BAR_DIM;
        break;
    case top_bar::Role::Separator:
        break;
    }
}

} // namespace

std::vector<top_bar::Tab> RenderEngine::top_tabs() const {
    std::vector<top_bar::Tab> tabs;
    const Window& active = tui_.win();
    for (const auto& wp : tui_.window_manager_->all()) {
        if (!wp || wp->kind != Window::Kind::Chat)
            continue;
        top_bar::Attention attention = top_bar::Attention::None;
        switch (wp->attention) {
        case Window::Attention::Done:
            attention = top_bar::Attention::Done;
            break;
        case Window::Attention::NeedsInput:
            attention = top_bar::Attention::NeedsInput;
            break;
        case Window::Attention::Error:
            attention = top_bar::Attention::Error;
            break;
        case Window::Attention::None:
            break;
        }
        tabs.push_back(
            {wp->id, wp->title, wp.get() == &active, tui_.runs_.busy(wp->id), attention});
    }
    return tabs;
}

int RenderEngine::top_right_width(const std::vector<Seg>& tops) const {
    int width = 0;
    for (const auto& s : tops)
        width += text::display_cols(s.text) + 1;
    return width;
}

void RenderEngine::draw_top_tabs(const top_bar::Layout& layout, int left_w) {
    int x = 0;
    for (const auto& span : layout.spans) {
        int pair = P_BANNER;
        int attrs = 0;
        top_role_style(span.role, pair, attrs);
        put_span(0, left_w, x, span.text, pair, attrs);
    }
}

void RenderEngine::draw_top_right(int w, const std::vector<Seg>& tops, bool colon) {
    const int right_w = top_right_width(tops);
    // One column of air at the edge: flush against it the last segment reads as
    // clipped by the window rather than as placed there.
    int x = std::max(0, w - right_w - 1);
    for (const auto& s : tops) {
        put_span(0, w, x, " ", P_BANNER, 0);
        const std::string text = s.id == "clock" ? top_bar::pulse_clock(s.text, colon) : s.text;
        put_span(0, w, x, text, s.pair, 0);
    }
}

void RenderEngine::put_menu_glyph(int row, int col, const std::string& glyph, int pair) {
    int x = col;
    put_span(row, col + 1, x, glyph, pair, 0);
}

void RenderEngine::put_menu_hline(int row, int col, int n, const std::string& glyph, int pair) {
    const std::wstring unit = to_wide(glyph);
    if (unit.empty() || n <= 0)
        return;
    const std::wstring line(static_cast<std::size_t>(n), unit.front());
    attron(COLOR_PAIR(pair));
    mvaddnwstr(row, col, line.c_str(), n);
    attroff(COLOR_PAIR(pair));
}

void RenderEngine::draw_menu_frame(const menu::Rect& box, bool utf8) {
    if (!box.visible() || box.w < 4 || box.h < 3)
        return;
    const std::string horizontal = utf8 ? "\u2500" : "-";
    const std::string vertical = utf8 ? "\u2502" : "|";
    const std::string top_left = utf8 ? "\u250c" : "+";
    const std::string top_right = utf8 ? "\u2510" : "+";
    const std::string bottom_left = utf8 ? "\u2514" : "+";
    const std::string bottom_right = utf8 ? "\u2518" : "+";

    attron(COLOR_PAIR(P_DIALOG));
    for (int row = box.y; row < box.y + box.h; ++row) {
        move(row, box.x);
        for (int i = 0; i < box.w; ++i)
            addch(' ');
    }
    attroff(COLOR_PAIR(P_DIALOG));

    put_menu_glyph(box.y, box.x, top_left, P_BAR_DIM);
    put_menu_hline(box.y, box.x + 1, box.w - 2, horizontal, P_BAR_DIM);
    put_menu_glyph(box.y, box.x + box.w - 1, top_right, P_BAR_DIM);
    put_menu_glyph(box.y + box.h - 1, box.x, bottom_left, P_BAR_DIM);
    put_menu_hline(box.y + box.h - 1, box.x + 1, box.w - 2, horizontal, P_BAR_DIM);
    put_menu_glyph(box.y + box.h - 1, box.x + box.w - 1, bottom_right, P_BAR_DIM);
    for (int row = box.y + 1; row < box.y + box.h - 1; ++row) {
        put_menu_glyph(row, box.x, vertical, P_BAR_DIM);
        put_menu_glyph(row, box.x + box.w - 1, vertical, P_BAR_DIM);
    }
}

void RenderEngine::draw_menu_separator(const menu::Rect& box, int row, bool utf8) {
    const std::string horizontal = utf8 ? "\u2500" : "-";
    put_menu_glyph(row, box.x, utf8 ? "\u251c" : "+", P_BAR_DIM);
    put_menu_hline(row, box.x + 1, box.w - 2, horizontal, P_BAR_DIM);
    put_menu_glyph(row, box.x + box.w - 1, utf8 ? "\u2524" : "+", P_BAR_DIM);
}

void RenderEngine::draw_menu_row(const menu::Rect& box, int row, const menu::Item& item,
                                 bool chosen, bool utf8) {
    const int pair = chosen ? P_BUTTON_ACT : P_DIALOG;
    const int attrs = chosen ? A_BOLD : 0;
    attron(COLOR_PAIR(pair));
    move(row, box.x + 1);
    for (int j = 0; j < box.w - 2; ++j)
        addch(' ');
    attroff(COLOR_PAIR(pair));
    const bool children = !item.children.empty();
    int x = box.x + 2;
    const int budget = box.x + box.w - 2 - (children ? 1 : 0);
    put_span(row, budget, x, std::string(" ") + item.label, pair, attrs);
    if (children) {
        int ax = box.x + box.w - 2;
        put_span(row, box.x + box.w - 1, ax, utf8 ? "\u25b8" : ">", pair, attrs);
    }
}

void RenderEngine::draw_menu_items(const menu::Rect& box, const std::vector<menu::Item>& items,
                                   std::size_t selected, bool utf8) {
    if (!box.visible() || box.w < 4 || box.h < 3)
        return;
    const int rows = box.h - 2;
    for (int i = 0; i < rows && i < static_cast<int>(items.size()); ++i) {
        const int row = box.y + 1 + i;
        const auto& item = items[static_cast<std::size_t>(i)];
        if (item.separator)
            draw_menu_separator(box, row, utf8);
        else
            draw_menu_row(box, row, item, static_cast<std::size_t>(i) == selected, utf8);
    }
}

void RenderEngine::draw_menu_box(const menu::Rect& box, const std::vector<menu::Item>& items,
                                 std::size_t selected, bool utf8) {
    draw_menu_frame(box, utf8);
    draw_menu_items(box, items, selected, utf8);
}

void RenderEngine::draw_menu_dropdown() {
    const menu::State& state = tui_.menu_->state();
    if (!tui_.menu_->is_open() || state.menu >= tui_.menu_->model().menus.size() ||
        !menu_layout_.dropdown.visible())
        return;
    const bool utf8 = agent::bar::utf8();
    const auto& items = tui_.menu_->model().menus[state.menu].items;
    draw_menu_box(menu_layout_.dropdown, items, state.item, utf8);
    if (state.mode == menu::Mode::Submenu && state.item < items.size() &&
        menu_layout_.submenu.visible())
        draw_menu_box(menu_layout_.submenu, items[state.item].children, state.submenu, utf8);
}

void RenderEngine::draw_menu_titles(int left_w, bool utf8) {
    menu_layout_ =
        menu::layout(tui_.menu_->model(), tui_.menu_->state(), left_w, top_rows(), height() - 2);
    int x = 0;
    put_span(0, left_w, x, top_bar::app_mark(utf8), P_BRAND, A_BOLD);
    for (const auto& title : menu_layout_.titles) {
        const auto& label = tui_.menu_->model().menus[title.menu].label;
        const int attrs = title.menu == tui_.menu_->state().menu ? (A_REVERSE | A_BOLD) : 0;
        int tx = title.rect.x;
        put_span(0, left_w, tx, std::string(" ") + label + " ", P_BANNER, attrs);
    }
}

void RenderEngine::draw_top_bar() {
    const int rows = top_rows();
    if (rows <= 0)
        return;
    const int w = width();
    const bool utf8 = agent::bar::utf8();

    attron(COLOR_PAIR(P_BANNER));
    mvhline(0, 0, ' ', w);
    attroff(COLOR_PAIR(P_BANNER));

    const auto tops = top_segments();
    const std::time_t now = std::time(nullptr);
    last_top_second_ = now;
    // The beat belongs to work in progress: the clock's colon blinks while a turn
    // or a job runs, and is solid when amber is idle. `colon` is "draw the colon",
    // so idle is always true -- inverting this printed "15 28" at rest.
    const bool colon = !tui_.runs_.any_busy() || top_bar::pulse_on(now);
    const int right_w = top_right_width(tops);
    const int left_w = std::max(0, w - right_w - (right_w > 0 ? 2 : 0));
    if (tui_.menu_->is_open()) {
        top_layout_ = {};
        draw_menu_titles(left_w, utf8);
    } else {
        menu_layout_ = {};
        top_layout_ = top_bar::layout(top_tabs(), left_w, utf8);
        draw_top_tabs(top_layout_, left_w);
    }
    draw_top_right(w, tops, colon);
}

void RenderEngine::tick_clock() {
    if (std::time(nullptr) == last_top_second_)
        return;
    dirty_ = true;
    draw_top_bar();
    wnoutrefresh(stdscr);
}

namespace {

int role_pair(input_line_layout::Role r) {
    switch (r) {
    case input_line_layout::Role::Branch:
        return P_ASSISTANT;
    case input_line_layout::Role::Plus:
        return P_GIT_PLUS;
    case input_line_layout::Role::Minus:
        return P_GIT_MINUS;
    case input_line_layout::Role::Project:
    case input_line_layout::Role::Decor:
    default:
        return P_USER;
    }
}

int role_attrs(input_line_layout::Role r) {
    return r == input_line_layout::Role::Decor ? A_DIM : 0;
}

} // namespace

void RenderEngine::put_input(int y, int w, int& x, const std::string& text, int pair, int attrs) {
    if (x >= w)
        return;
    std::wstring ws = to_wide(text);
    const int room = w - x;
    if (static_cast<int>(ws.size()) > room)
        ws.resize(static_cast<std::size_t>(room));
    if (attrs)
        attron(COLOR_PAIR(pair) | attrs);
    else
        attron(COLOR_PAIR(pair));
    mvaddnwstr(y, x, ws.c_str(), static_cast<int>(ws.size()));
    if (attrs)
        attroff(COLOR_PAIR(pair) | attrs);
    else
        attroff(COLOR_PAIR(pair));
    x += display_cols(text);
}

namespace {

// Where the terminal cursor sits, clamped into the line.
int cursor_column(int cursor_col, int scroll_off, int w) {
    int cx = cursor_col - scroll_off;
    if (cx < 0)
        cx = 0;
    if (cx >= w)
        cx = w - 1;
    return cx;
}

// Paint the visible slice of the input, after the prompt, in the user pair.
void draw_input_slice(int y, int w, int prompt_w, int scroll_off, const std::string& s) {
    const int input_start = std::max(0, prompt_w - scroll_off);
    if (input_start >= w)
        return;
    const std::size_t skip =
        scroll_off > prompt_w ? text::col_to_byte(s, scroll_off - prompt_w) : 0;
    const int input_len = input_line_layout::visible_bytes(s, skip, w - input_start);
    if (input_len <= 0)
        return;
    attron(COLOR_PAIR(P_USER));
    mvaddnstr(y, input_start, s.c_str() + skip, input_len);
    attroff(COLOR_PAIR(P_USER));
}

} // namespace

void RenderEngine::draw_input(const std::string& s, size_t cursor, const std::string& shadow) {
    dirty_ = true;
    draw_drawer(s);
    const int y = height() - 1;
    const int w = width();
    int x = 0;

    move(y, 0);
    clrtoeol();

    const auto git = git_state(); // one immutable snapshot for this frame
    for (const auto& piece : input_line_layout::prompt_pieces(
             git ? git->project : std::string(), git ? git->branch : std::string(),
             git ? git->ins : 0, git ? git->del : 0))
        put_input(y, w, x, piece.text, role_pair(piece.role), role_attrs(piece.role));

    const int prompt_w = x;
    const input_line_layout::Scroll sc =
        input_line_layout::scroll_for(prompt_w, s, cursor, shadow, w);
    draw_input_slice(y, w, prompt_w, sc.offset, s);
    draw_input_shadow(y, w, prompt_w, sc.offset, s, cursor, shadow);

    if (s.empty() && shadow.empty() && !tui_.menu_->hint().empty()) {
        int hx = std::max(0, prompt_w - sc.offset);
        put_input(y, w, hx, tui_.menu_->hint(), P_HINT);
        curs_set(0);
        wnoutrefresh(stdscr);
        return;
    }
    if (tui_.menu_->is_open()) {
        curs_set(0);
        wnoutrefresh(stdscr);
        return;
    }
    curs_set(1);
    move(y, cursor_column(sc.cursor_col, sc.offset, w));
    wnoutrefresh(stdscr);
}

void RenderEngine::draw_input_shadow(int y, int w, int prompt_w, int scroll_off,
                                     const std::string& input, std::size_t cursor,
                                     const std::string& shadow) {
    if (shadow.empty() || cursor != input.size())
        return;
    const int input_w = prompt_w + display_cols(input);
    const int shadow_start = input_w - scroll_off;
    if (shadow_start < 0 || shadow_start >= w)
        return;
    attron(A_DIM | COLOR_PAIR(P_INPUT_SHADOW));
    mvaddnstr(y, shadow_start, shadow.c_str(),
              std::min(static_cast<int>(shadow.size()), w - shadow_start));
    attroff(A_DIM | COLOR_PAIR(P_INPUT_SHADOW));
}

namespace {

// Blank the rows in [from, to).
void clear_rows(int from, int to) {
    for (int row = from; row < to; ++row) {
        move(row, 0);
        clrtoeol();
    }
}

} // namespace

void RenderEngine::draw_drawer(const std::string& input) {
    // The flag mirrors CommandLine's drawer state, but the line is the fact: a
    // closed and idle UI repaints the input every tick, and painting a drawer
    // from text that is no longer a command is how an empty line reached the
    // parser (and killed the process from the idle tick).
    if (!drawer_open_ || input.empty() || input.front() != '/')
        return;
    const int bar_row = height() - 2;
    if (modal_open()) {
        clear_rows(std::max(0, bar_row - 8), bar_row);
        return;
    }

    const std::vector<std::string> rows = drawer_rows(input, tui_.settings_, drawer_provider_);

    constexpr int header = 1;
    const int max_rows = std::max(1, bar_row - chat_top());
    // Clamp into range first, then window, so the painted highlight and the
    // dispatched index are always derived from the same selection.
    drawer_sel_ =
        std::min(std::max(0, drawer_sel_), std::max(0, static_cast<int>(rows.size()) - 1));
    const auto win = drawer_window(static_cast<int>(rows.size()), max_rows - header, drawer_sel_);
    const int top = bar_row - header - win.second;

    DrawerView view;
    view.header = " options  (Tab complete  Up/Down select  Enter run  ? help  Esc cancel) ";
    view.tabs = drawer_tabs_;
    view.rows.assign(rows.begin() + win.first, rows.begin() + win.first + win.second);
    view.selected = drawer_sel_ - win.first;

    // The drawer is one self-contained strip. The shell already owns stdscr, so
    // blit into that buffer; draw_input's wnoutrefresh and the loop's doupdate
    // present it with the rest of the frame.
    const Surface surface = paint_drawer(view, width());
    terminal_.blit(surface, 0, top);
}

void RenderEngine::request_git_refresh(const std::string& root) {
    // Non-blocking: one status read (plus a diff when tracked) on a detached
    // worker, at most once per window. A burst of tool results coalesces; the
    // UI thread never forks. One refresh at a time; a request that arrives
    // while one is in flight is satisfied by it. The root is the ACTIVE
    // window's workspace, so the bar reports the tree the user is working in.
    const auto now = std::chrono::steady_clock::now();
    if (!text::git_refresh_due(last_git_request_, now))
        return;
    last_git_request_ = now;
    if (git_pub_->in_flight.exchange(true))
        return;
    auto pub = git_pub_; // by value: the worker never touches this RenderEngine
    std::thread([pub, root] {
        auto next = std::make_shared<GitState>(read_git_state(root));
        std::atomic_store_explicit(&pub->state, std::shared_ptr<const GitState>(std::move(next)),
                                   std::memory_order_release);
        pub->in_flight.store(false);
    }).detach();
}

std::shared_ptr<const RenderEngine::GitState> RenderEngine::git_state() const {
    return std::atomic_load_explicit(&git_pub_->state, std::memory_order_acquire);
}

RenderEngine::GitState RenderEngine::read_git_state(const std::string& root) {
    GitState st;
    st.project = current_project_name(root);

    // One process for branch, oid and every entry class. The pathspec matches
    // the diff's exclusions, so the two reads describe the same tree.
    const text::GitStatus parsed = git_reads::read_status(root);
    st.branch = parsed.branch;
    if (st.branch.empty())
        return st;

    // A diff read is only needed when the status reported tracked changes;
    // untracked-only work is the `*` case below.
    if (parsed.tracked) {
        const text::DiffStat stat = git_reads::read_diff_stat(root);
        st.ins = stat.ins;
        st.del = stat.del;
    }
    st.dirty = parsed.dirty;
    if (st.dirty && st.ins == 0 && st.del == 0)
        st.branch += "*";
    return st;
}

} // namespace tui
