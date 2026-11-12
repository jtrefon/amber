#ifndef AMBER_TUI_RENDER_ENGINE_H
#define AMBER_TUI_RENDER_ENGINE_H

#include "tui/drawer_tabs.h"
#include "tui/frame_painter.h"
#include "tui/terminal.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "text_canvas.h"
#include "input_line_layout.h"
#include "menu_view.h"
#include "top_bar.h"
#include "markdown.h"
#include "rich.h"
#include "palette.h"
#include "status_bar_layout.h"

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "agent/statusbar.h"

namespace tui {
class Tui;
struct Window;

class RenderEngine {
public:
    explicit RenderEngine(Tui& tui);

    int height() const;
    int width() const;
    // The top rail is two rows (tabs + rule) on a normal terminal, one row on a
    // short one, and absent when there is no room above the status/input pair.
    int top_rows() const;
    // The session tab under a screen cell, when the top rail owns that row.
    std::optional<std::size_t> tab_at(int x, int y) const;
    // The close cell inside a tab, when the click landed on its `×`.
    std::optional<std::size_t> tab_close_at(int x, int y) const;
    // The menu title/item/dropdown under a screen cell.
    std::optional<menu::Hit> menu_hit(int x, int y) const;
    int chat_top() const;
    int chat_height() const;
    int lines_per_page() const;
    int max_scroll(const Window& w) const;
    int max_scroll() const;
    std::vector<rich::Line> build_view_without_working(const Window& w) const;
    // The live thinking block, and the in-flight assistant text.
    void append_reasoning(std::vector<rich::Line>& view, const Window& w) const;
    void append_stream_preview(std::vector<rich::Line>& view, const Window& w) const;

    static size_t utf8_len(const std::string& s, size_t i);
    static std::vector<std::string> wrap_text(const std::string& text, int w);
    static void append_rich_to(std::vector<rich::Line>& view, const std::string& text, int color,
                               int w);
    void append_rich_to(Window& w, const rich::Line& l);

    void draw();
    // Repaint only the rows that animate while a run is live: the working line
    // and the status bar. The transcript keeps its 150 ms cadence -- a pulse is
    // not a reason to rewrap and repaint it.
    void tick_pulse();
    // The working line under the chat.
    void draw_working_indicator(int row);
    // Status-bar painting: one segment (clipped to the budget), the moving
    // highlight, the context gauge, and the right zone.
    void put_segment(int y, int budget, int& x, const std::string& s, int pair);
    // The same for a string that is already wide: the gauge and the verb are
    // sliced by display column, which is a wide-string index.
    void put_wsegment(int y, int budget, int& x, const std::wstring& s, int pair);
    // Paint `cells` one per display column, shading the moving band over it.
    // `shine` carries the band's width in cells (0 = a third of the run) and
    // whether it fades or is one solid shade.
    struct Shine {
        int band = 0;
        agent::bar::ShineStyle style = agent::bar::ShineStyle::Gradient;
    };
    void draw_shine(int y, int budget, int& x, const std::wstring& cells, int base_pair,
                    Shine shine);
    // The window the ACTIVE agent will actually use. Reading the template
    // (tui_.cfg_) is what made the gauge stay at a stale 130k after a switch.
    int context_window() const;
    void draw_ctx_gauge(int y, int budget, int& x, bool have_ctx, long ctx_used, double frac);
    void draw_right_zone(int y, int w, int right_w,
                         const std::vector<status_bar_layout::Segment>& right_zone);
    void draw_status_bar();
    // The top rail: bare amber mark, capped session tabs, health pulse, clock
    // and the icon-only right tray. The bottom bar paints itself.
    void draw_top_bar();
    // The row the working line occupies, when it is shown.
    int working_row() const;
    // Advance the 150 ms frame the spin glyph steps through. The sweep itself is
    // time-based (anim_phase01), so its speed cannot drift with the paint rate.
    void tick_anim();
    // Position of the sweep in [0,1), from the wall clock.
    double anim_phase01() const;
    // The pair for a shine level over `base_pair`: level 0 keeps the base shade.
    static int shine_pair(int base_pair, int level);
    // Repaint the bar on the second. A segment that renders wall-clock time
    // (the clock plugin) is correct because of this cadence, not because it
    // schedules anything itself: segments are pure reads of the moment they
    // are painted.
    void tick_clock();
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    void draw_drawer(const std::string& input);
    void sync_chat_canvas(bool show_working) const;
    void begin_frame();
    // Flush the frame. A full draw leaves the cursor alone: the input line owns
    // its visibility and position, and hiding then re-showing it on every
    // animation frame resets the terminal's blink timer (it blinks fast).
    void present_frame(bool update_cursor = true);

    static std::string drawer_token(const std::string& input);
    std::vector<const palette::Command*> filter_commands(const std::string& token);

    // Git state for the prompt (project, branch, diff counts). Refresh runs on a
    // detached worker: git is a subprocess and the UI thread must never fork, so
    // request_git_refresh() returns immediately and the renderer reads whatever
    // snapshot is published. The worker holds the publication by shared_ptr, so a
    // refresh in flight during teardown writes into live memory, not into a
    // destroyed RenderEngine.
    struct GitState {
        std::string project;
        std::string branch;
        int ins = 0;
        int del = 0;
        bool dirty = false; // tracked or untracked work beyond the diff counts
    };
    void request_git_refresh(const std::string& root);
    std::shared_ptr<const GitState> git_state() const;
    md::Style& md_style() noexcept { return md_style_; }

    // UI-state accessors (state lives here; Tui facade forwards).
    bool dirty() const noexcept { return dirty_; }
    void mark_dirty() noexcept { dirty_ = true; }
    void clear_dirty() noexcept { dirty_ = false; }
    void flush() { terminal_.flush(); }
    std::chrono::steady_clock::time_point last_status_tick() const noexcept {
        return last_status_tick_;
    }
    void set_last_status_tick(std::chrono::steady_clock::time_point t) noexcept {
        last_status_tick_ = t;
    }
    bool drawer_open() const noexcept { return drawer_open_; }
    void set_drawer_open(bool v) noexcept { drawer_open_ = v; }
    void set_theme(const Theme& theme) { terminal_.set_theme(theme); }
    void set_reduced_motion(bool reduced) noexcept { reduced_motion_ = reduced; }
    bool reduced_motion() const noexcept { return reduced_motion_; }
    int drawer_sel() const noexcept { return drawer_sel_; }
    void set_drawer_sel(int v) noexcept { drawer_sel_ = v; }
    void set_drawer_tabs(DrawerTabStrip strip) noexcept { drawer_tabs_ = std::move(strip); }
    const DrawerTabStrip& drawer_tabs() const noexcept { return drawer_tabs_; }
    // The provider the selected tab shows; empty = All. The rows painted are
    // narrowed to it, so the list on screen is the list Enter dispatches from.
    void set_drawer_provider(std::string provider) noexcept {
        drawer_provider_ = std::move(provider);
    }
    bool scroll_mode() const noexcept { return scroll_mode_; }
    void set_scroll_mode(bool v) noexcept { scroll_mode_ = v; }
    bool show_reasoning() const noexcept { return show_reasoning_; }
    void set_show_reasoning(bool v) noexcept { show_reasoning_ = v; }
    bool working_visible() const noexcept { return working_visible_; }
    void mark_working() noexcept;
    void clear_working() noexcept { working_visible_ = false; }
    int anim_phase() const noexcept { return anim_phase_; }

private:
    // A status segment is the L1 layout's type: the bar is laid out purely
    // (status_bar_layout) and this class only paints the plan.
    using Seg = status_bar_layout::Segment;
    std::vector<Seg> bar_segments() const;
    std::vector<Seg> top_segments() const;
    std::vector<top_bar::Tab> top_tabs() const;
    int top_right_width(const std::vector<Seg>& tops) const;
    void draw_top_tabs(const top_bar::Layout& layout, int left_w);
    void draw_top_right(int w, const std::vector<Seg>& tops, bool colon);
    void draw_menu_dropdown();
    void draw_menu_titles(int left_w, bool utf8);
    void draw_menu_box(const menu::Rect& box, const std::vector<menu::Item>& items,
                       std::size_t selected, bool utf8);
    void draw_menu_frame(const menu::Rect& box, bool utf8);
    void draw_menu_items(const menu::Rect& box, const std::vector<menu::Item>& items,
                         std::size_t selected, bool utf8);
    void draw_menu_row(const menu::Rect& box, int row, const menu::Item& item, bool chosen,
                       bool utf8);
    void draw_menu_separator(const menu::Rect& box, int row, bool utf8);
    void put_menu_glyph(int row, int col, const std::string& glyph, int pair);
    void put_menu_hline(int row, int col, int n, const std::string& glyph, int pair);
    agent::StatusSnapshot build_status_snapshot() const;
    static int tone_pair(agent::StatusTone tone);
    static int display_cols(const std::string& s);
    static std::wstring to_wide(const std::string& s);
    static int gauge_pair(double f);
    // Draw one input-line piece at `x`, advancing it (truncated to the width).
    void put_input(int y, int w, int& x, const std::string& text, int pair, int attrs = 0);
    // The same for top-rail spans, which carry an attribute as well as a pair.
    void put_span(int y, int w, int& x, const std::string& text, int pair, int attrs);
    // Draw the dim completion hint after the input, when the cursor is at the end.
    void draw_input_shadow(int y, int w, int prompt_w, int scroll_off, const std::string& input,
                           std::size_t cursor, const std::string& shadow);

    // The activity word leading the working indicator ("thinking", "talking",
    // "compressing", "searching", "working", ...) derived from the agent's
    // current run state, the in-flight tool, and whether compression is
    // running.
    std::string activity_verb() const;

    Tui& tui_;
    mutable TextCanvas chat_canvas_;
    md::Style md_style_;
    // Worker-side: the prompt's only git entry point (git_reads owns the
    // subprocesses). current_project_name() is a getcwd (no fork), used to seed
    // the prompt before the first refresh lands.
    static GitState read_git_state(const std::string& root);
    // The display name of a tree: its last path component, or the process's
    // working directory when root is empty.
    static std::string current_project_name(const std::string& root = "");

    struct GitPublication {
        std::shared_ptr<const GitState> state;
        std::atomic<bool> in_flight{false};
    };
    std::shared_ptr<GitPublication> git_pub_ = std::make_shared<GitPublication>();
    std::chrono::steady_clock::time_point last_git_request_{};
    bool drawer_open_ = false;
    int drawer_sel_ = 0;
    // The model picker's tab strip, empty when the open drawer has no tabs.
    DrawerTabStrip drawer_tabs_;
    Terminal terminal_;
    Surface frame_{0, 0};
    FramePainter paint_;
    // The provider the selected tab narrows the painted rows to ("" = All).
    std::string drawer_provider_;
    bool scroll_mode_ = false;
    bool show_reasoning_ = true;
    int anim_phase_ = 0;
    std::chrono::steady_clock::time_point anim_tick_{};
    // The sweep's epoch: the pulse reads a position from the wall clock, so it
    // keeps moving even when a paint is skipped or the frame counter is stale.
    std::chrono::steady_clock::time_point pulse_epoch_{};
    std::chrono::steady_clock::time_point working_since_{};
    bool working_visible_ = false;
    // Whether the last draw sized the canvas to leave the working row free, so
    // tick_pulse may repaint that row without resizing anything.
    bool working_row_owned_ = false;
    bool reduced_motion_ = false;
    bool dirty_ = true;
    std::chrono::steady_clock::time_point last_status_tick_{};
    // The top rail changes on the second (clock + pulse), not on every idle
    // tick. Cached so the 150 ms loop does not repaint it seven times.
    std::time_t last_top_second_ = 0;
    // The last painted rail, kept for mouse hit-testing. Repainted whenever the
    // tabs or the width change, so it cannot lag a click behind the screen.
    top_bar::Layout top_layout_;
    // The last painted menu surface, kept for mouse hit-testing.
    menu::Layout menu_layout_;
};

} // namespace tui

#endif