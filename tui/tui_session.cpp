
#include "tui.h"
#include "tui/dialog.h"
#include "tui/confirm_panel.h"
#include "tui/session_browser_core.h"
#include "tui/session_row.h"
#include "tui/keys_ncurses.h"
#include "tui/window_ops.h"
#include "tool_display.h"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <string>
#include <vector>

namespace tui {

// Render one restored session message as scrollback lines. Assistant
// tool_calls queue RestoredCall entries; tool messages emit a single
// timestamped result line (describe + summary, no exit status).
void SessionController::restore_message_lines(const agent::Message& m,
                                              std::vector<RestoredCall>& pending) {
    Window& w = tui_.win();
    if (m.role == "user") {
        tui_.append_line(P_USER, "> " + m.content);
        return;
    }
    if (m.role == "assistant") {
        if (!m.tool_calls.is_null() && !m.tool_calls.empty()) {
            for (const auto& tc : m.tool_calls) {
                RestoredCall c;
                auto fn = tc.value("function", agent::json::object());
                c.name = fn.value("name", "?");
                auto args = fn.value("arguments", agent::json::object());
                if (args.is_string()) {
                    auto parsed = agent::json::parse(args.get<std::string>(), nullptr, false);
                    args = parsed.is_discarded() ? agent::json::object() : std::move(parsed);
                }
                c.args = std::move(args);
                pending.push_back(std::move(c));
            }
        }
        if (!m.content.empty())
            tui_.append_markdown(w, m.content);
        return;
    }
    if (m.role == "tool") {
        if (!pending.empty()) {
            RestoredCall c = std::move(pending.front());
            pending.erase(pending.begin());
            rich::Line ln =
                tool_display::result_line(c.name, c.args, true, m.content, "", tui_.reg_);
            rich::Run ts;
            ts.text = Tui::timestamp();
            ts.pair = P_REASONING;
            ts.dim = true;
            ln.runs.insert(ln.runs.begin(), std::move(ts));
            tui_.append_rich(ln);
        } else {
            std::string preview = m.content;
            if (preview.size() > 80) {
                preview.resize(77);
                preview += "...";
            }
            tui_.append_line(P_STATUS, "  \u2514 " + m.name + ": " + preview);
        }
    }
}

agent::Session SessionController::snapshot(Window& w) const {
    agent::Session s;
    s.id = w.session_id;
    // The session file describes THIS window: model and telemetry come from
    // its own agent/run state, never the global template or a sibling.
    const auto acfg = w.agent ? w.agent->config_snapshot() : nullptr;
    s.model = acfg ? acfg->model : tui_.cfg_.model;
    if (w.agent) {
        const auto& ctx = w.agent->context().get_all();
        s.messages.assign(ctx.begin(), ctx.end());
        s.meta = w.agent->meta_;
    }
    // Persist UI state so it survives exit/reload.
    s.meta["ctx_used"] = w.ctx_used.load();
    s.meta["ctx_size"] = acfg ? acfg->context_size : tui_.cfg_.context_size;
    if (w.stats.valid) {
        s.meta["latency_ms"] = w.stats.latency_ms;
        s.meta["tps"] = w.stats.tps;
        s.meta["prompt_tokens"] = w.stats.prompt_tokens;
        s.meta["completion_tokens"] = w.stats.completion_tokens;
    }
    s.derive_title();
    if (w.title != "chat" && !w.title.empty())
        s.title = w.title;
    return s;
}

void SessionController::autosave() {
    autosave(tui_.win());
}

void SessionController::autosave(Window& w) {
    if (!w.dirty || !w.agent || w.agent->context().empty())
        return;
    // Called from on_done (agent finished — quiescent) and shutdown paths,
    // never mid-run. The single-owner rule is respected by the callers: the
    // context is only snapshotted here when the worker is not mutating it.
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        if (w.title == "chat" && !s.title.empty())
            w.title = s.title;
        w.dirty = false;
    }
}

void SessionController::save_session() {
    Window& w = tui_.win();
    if (!w.agent || w.agent->context().empty()) {
        tui_.append_line(P_STATUS, "nothing to save (empty conversation)");
        return;
    }
    // Single-owner rule (see autosave): never snapshot a context the worker
    // is mid-mutation on. A background compression sets busy for its whole
    // run; saving then would race the rebuild.
    if (tui_.runs_.busy(w.id)) {
        tui_.append_line(P_STATUS, "save deferred \u2014 agent is busy (finishes and auto-saves)");
        return;
    }
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        w.dirty = false;
        tui_.append_line(P_STATUS, "saved session " + s.id + " (\"" + s.title + "\")");
    } else {
        tui_.append_line(P_STATUS, "save failed (could not write " + store_.dir() + ")");
    }
}

void SessionController::fork_session() {
    Window& src = tui_.win();
    if (!src.agent || src.agent->context().empty()) {
        tui_.append_line(P_STATUS, "fork: nothing to fork (empty conversation)");
        return;
    }
    // Context is single-owner: copying it while the worker mutates it would
    // race, and get_all()'s chain assert can trip on a half-pushed message.
    if (tui_.runs_.busy(src.id)) {
        tui_.append_line(P_STATUS, "fork deferred — agent is busy (fork when idle)");
        return;
    }
    // The fork's wire prefix is byte-identical to the source's (system
    // prompt sealed at context[0], same model/mode on the copied config),
    // so its first request reuses the server KV cache for the whole shared
    // history. It gets its own session id and transcript: one store row per
    // leg keeps the two diverging histories independently restorable.
    Window& fork = tui_.new_window(src.title + " ⑂");
    fork.agent->fork_from(*src.agent);
    fork.agent->meta_["forked_from"] = src.session_id.empty() ? src.title : src.session_id;
    fork.dirty = true;
    autosave(src);
    autosave(fork);
    tui_.append_line(P_STATUS, "forked session into window '" + fork.title + "'");
}

void SessionController::load_session(const std::string& id) {
    agent::Session s;
    if (!store_.load(id, s)) {
        tui_.append_line(P_STATUS, "load failed: no session " + id);
        return;
    }
    Window& w = tui_.new_window(s.title.empty() ? "chat" : s.title);
    w.session_id = s.id;
    w.agent->set_context(s.messages);
    // Large context on load?  Compress asynchronously so the first turn uses a
    // smaller prefill.  We check utilisation directly (not the per-turn gate)
    // because this is a one-time load reduction, not an inline compression that
    // would break tail-injection.
    double utilisation = s.messages.empty()
                             ? 0.0
                             : static_cast<double>(w.agent->context().token_count()) /
                                   std::max(1, tui_.cfg_.context_size);
    if (utilisation > 0.40) {
        tui_.append_line(P_STATUS, "large session — background compression started");
        tui_.switch_to(tui_.window_manager_->all().size() - 1);
        Window* my_win = &w;
        size_t my_id = w.id;
        tui_.compress_worker(*my_win, my_id);
    }
    if (!s.meta.empty())
        w.agent->meta_ = s.meta;
    // Restore UI state from saved session meta.
    auto get_num = [&](const char* key, long def) -> long {
        return (s.meta.contains(key) && s.meta[key].is_number()) ? s.meta[key].get<long>() : def;
    };
    w.ctx_used.store(get_num("ctx_used", -1));
    w.ctx_estimate = 0; // refilled by the restore's context events
    long restored_ctx = get_num("ctx_size", 0);
    if (restored_ctx > 0)
        tui_.cfg_.context_size = static_cast<int>(restored_ctx);
    if (s.meta.contains("latency_ms") && s.meta["latency_ms"].is_number()) {
        w.stats.latency_ms = s.meta["latency_ms"].get<double>();
        w.stats.tps = static_cast<double>(get_num("tps", -1));
        w.stats.prompt_tokens = get_num("prompt_tokens", -1);
        w.stats.completion_tokens = get_num("completion_tokens", -1);
        w.stats.valid = true;
    }
    std::vector<SessionController::RestoredCall> pending;
    for (const auto& m : s.messages)
        restore_message_lines(m, pending);
    if (!s.messages.empty()) {
        tui_.win().scroll_top = tui_.render_engine_->max_scroll();
    }
    tui_.append_line(P_STATUS, "loaded session " + s.id);
    tui_.draw();
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

} // namespace

static void draw_session_rows(WINDOW* w, SessionBrowserCore& core, int aw);
static void draw_date_header(WINDOW* w, int row, const BrowserItem& m);
static void draw_search_bar(WINDOW* w, int ah, int aw, const std::string& filter);
static void draw_scroll_indicators(WINDOW* w, int aw, const SessionBrowserCore& core);

void SessionController::session_browser() {
    auto all = store_.list();
    if (all.empty()) {
        tui_.append_line(P_STATUS, "no saved sessions");
        return;
    }

    std::vector<BrowserItem> items;
    items.reserve(all.size());
    for (const auto& m : all)
        items.push_back({m.id, m.title, m.updated_ms, m.model, m.message_count, m.file_size});

    const session_row::Size sz =
        session_row::dialog_size(tui_.render_engine_->height(), tui_.render_engine_->width());
    Dialog dlg(sz.dh, sz.dw, "Sessions");
    dlg.set_footer({{"Up/Down", "nav"},
                    {"Enter", "load"},
                    {"Del", "remove"},
                    {"/", "search"},
                    {"Esc", "back"}});
    WINDOW* w = dlg.win();
    const int aw = dlg.cols() - 2; // content width
    const int ah = dlg.rows() - 2; // content height

    // All list/filter/selection semantics live in the core (which is what the
    // unit tests exercise); this loop only paints it and acts on its verdicts.
    SessionBrowserCore core(std::move(items), browser_layout(dlg.rows()).list_h);
    curs_set(1); // visible cursor for the search bar

    bool done = false;
    while (!done) {
        draw_session_rows(w, core, aw);
        draw_search_bar(w, ah, aw, core.filter());
        draw_scroll_indicators(w, aw, core);

        update_panels();
        doupdate();

        if (session_browser_key(wgetch(w), core))
            done = true;
    }

    curs_set(1);
    tui_.draw();
}

static void draw_session_rows(WINDOW* w, SessionBrowserCore& core, int aw) {
    // Render list — clear each row with its own attribute so highlights extend
    // full-width. The date header rows and blank rows use the dialog background
    // pair inherited from wbkgd.
    const int title_w = std::max(16, aw - 34);
    for (int i = 0; i < core.list_h(); ++i) {
        const int row = 1 + i;
        const int disp_idx = core.scroll_off() + i;
        const int kind = core.display_kind(disp_idx);
        if (kind < 0)
            continue; // past end — wbkgd shows through

        const BrowserItem& m = core.item(core.display_item(disp_idx));
        if (kind == 0) {
            draw_date_header(w, row, m);
            continue;
        }
        const bool cur = (disp_idx == core.sel());
        if (cur) {
            wattron(w, A_REVERSE | COLOR_PAIR(P_DIALOG));
            mvwaddstr(w, row, 1, std::string(aw, ' ').c_str());
        } else {
            wattron(w, COLOR_PAIR(P_ASSISTANT));
        }
        mvwaddstr(w, row, 1, "  ");
        int x = 3;
        const std::string t = session_row::title(m.title, title_w);
        mvwaddnstr(w, row, x, t.c_str(), title_w);
        x += title_w + 1;
        const std::string mod = session_row::model(m.model);
        mvwaddstr(w, row, x, mod.c_str());
        x += static_cast<int>(mod.size()) + 1;
        const std::string cnt = session_row::message_count(m.message_count);
        mvwaddstr(w, row, x, cnt.c_str());
        x += static_cast<int>(cnt.size()) + 1;
        const std::string size = session_row::file_size(m.file_size);
        if (!size.empty())
            mvwaddstr(w, row, x, size.c_str());
        const std::string ts = fmt_time(m.updated_ms);
        mvwaddstr(w, row, aw - static_cast<int>(ts.size()) + 1, ts.c_str());
        if (cur)
            wattroff(w, A_REVERSE | COLOR_PAIR(P_DIALOG));
        else
            wattroff(w, COLOR_PAIR(P_ASSISTANT));
    }
}

static void draw_date_header(WINDOW* w, int row, const BrowserItem& m) {
    wattron(w, COLOR_PAIR(P_BAR_DIM) | A_BOLD);
    mvwaddstr(w, row, 1, ("  " + date_label(m.updated_ms)).c_str());
    wattroff(w, COLOR_PAIR(P_BAR_DIM) | A_BOLD);
}

static void draw_search_bar(WINDOW* w, int ah, int aw, const std::string& filter) {
    const std::string search_prompt = "/ " + filter;
    wattron(w, COLOR_PAIR(P_STATUS));
    mvwaddstr(w, ah - 1, 1, std::string(aw, ' ').c_str());
    mvwaddnstr(w, ah - 1, 1, search_prompt.c_str(), aw);
    wmove(w, ah - 1, 2 + static_cast<int>(filter.size()));
    wattroff(w, COLOR_PAIR(P_STATUS));
}

static void draw_scroll_indicators(WINDOW* w, int aw, const SessionBrowserCore& core) {
    if (core.scroll_off() > 0)
        mvwaddch(w, 1, aw, ACS_UARROW);
    if (core.scroll_off() + core.list_h() < core.display_count())
        mvwaddch(w, core.list_h(), aw, ACS_DARROW);
}

bool SessionController::session_browser_key(int ch, SessionBrowserCore& core) {
    const auto r = core.key(ch);
    if (r.action == SessionBrowserCore::Result::Action::Accept) {
        if (int idx = core.load_index(); idx >= 0)
            load_session(core.item(idx).id);
        return true;
    }
    if (r.action == SessionBrowserCore::Result::Action::Cancel)
        return true;
    if (r.delete_pending && confirm_delete_session(core))
        return true;
    return false;
}

bool SessionController::confirm_delete_session(SessionBrowserCore& core) {
    const int idx = core.load_index();
    if (idx < 0)
        return false;
    std::string msg = "Delete \"" + core.item(idx).title + "\"?";
    tui::ConfirmPanel confirm("Delete Session", msg);
    if (!confirm.run())
        return false;
    store_.remove(core.item(idx).id);
    core.erase_current();
    if (core.display_count() == 0) {
        tui_.append_line(P_STATUS, "no saved sessions");
        return true;
    }
    return false;
}

void SessionController::save_window_sessions() {
    for (const auto& window : tui_.window_manager_->all()) {
        Window& w = *window;
        // Context is single-owner: never snapshot one the worker is mutating.
        if (tui_.runs_.busy(w.id))
            continue;
        if (!w.dirty || !w.agent || w.agent->context().get_all().empty())
            continue;
        std::fprintf(stderr, "\rsaving session '%s'...", w.title.c_str());
        std::fflush(stderr);
        agent::Session s = snapshot(w);
        if (store_.save(s))
            w.session_id = s.id;
    }
    std::fprintf(stderr, "\rsession save complete\n");
}

void Tui::lazy_load_active() {
    auto& w = win();
    if (!w.agent || !w.agent->context().empty() || w.session_id.empty())
        return;
    agent::Session s;
    if (!session_controller_->store().load(w.session_id, s)) {
        w.session_id.clear();
        return;
    }
    w.agent->set_context(s.messages);
    // Restore meta and UI state (same logic as load_session).
    if (!s.meta.empty())
        w.agent->meta_ = s.meta;
    auto get_num = [&](const char* key, long def) -> long {
        return (s.meta.contains(key) && s.meta[key].is_number()) ? s.meta[key].get<long>() : def;
    };
    w.ctx_used.store(get_num("ctx_used", -1));
    w.ctx_estimate = 0; // refilled by the restore's context events
    long restored_ctx = get_num("ctx_size", 0);
    if (restored_ctx > 0)
        cfg_.context_size = static_cast<int>(restored_ctx);
    if (s.meta.contains("latency_ms") && s.meta["latency_ms"].is_number()) {
        w.stats.latency_ms = s.meta["latency_ms"].get<double>();
        w.stats.tps = static_cast<double>(get_num("tps", -1));
        w.stats.prompt_tokens = get_num("prompt_tokens", -1);
        w.stats.completion_tokens = get_num("completion_tokens", -1);
        w.stats.valid = true;
    }
    w.lines.clear();
    // Spinner rows belong to the old scrollback — drop only this window's.
    {
        auto& pts = router_->pending_tools();
        pts.erase(std::remove_if(pts.begin(), pts.end(),
                                 [&w](const PendingToolLine& pt) { return pt.window_id == w.id; }),
                  pts.end());
    }
    std::vector<SessionController::RestoredCall> pending;
    for (const auto& m : s.messages)
        session_controller_->restore_message_lines(m, pending);
    if (!s.messages.empty())
        win().scroll_top = render_engine_->max_scroll();
}

void Tui::switch_to(size_t idx) {
    window_ops_->switch_to(idx);
}

void Tui::close_window() {
    window_ops_->close_window();
}

void SessionController::save_workspace_now() {
    agent::WorkspaceState ws;
    for (const auto& w : tui_.window_manager_->all()) {
        agent::WorkspaceState::WindowEntry we;
        we.session_id = w->session_id;
        we.title = w->title;
        we.prompt_history = w->prompt_history;
        ws.windows.push_back(we);
    }
    ws.active = tui_.window_manager_->active();
    store_.save_workspace(ws);
}
void SessionController::redraw_after_modal() {
    tui_.modal_open_ = false;
    // Resolve any approvals that arrived while a modal dialog was open. Each
    // resolve shows its own (non-nested) approval dialog on the now-live loop.
    while (!tui_.router_->pending_approvals().empty()) {
        AgentEvent ev = std::move(tui_.router_->pending_approvals().front());
        tui_.router_->pending_approvals().pop();
        tui_.router_->resolve_approval(ev);
    }
    // Same for API-key requests that arrived during a modal.
    while (!tui_.router_->pending_api_keys().empty()) {
        AgentEvent ev = std::move(tui_.router_->pending_api_keys().front());
        tui_.router_->pending_api_keys().pop();
        tui_.router_->resolve_api_key(ev);
    }
    touchwin(stdscr);
    tui_.draw();
    tui_.render_engine_->draw_input("");
    tui_.flush();
}

} // namespace tui
