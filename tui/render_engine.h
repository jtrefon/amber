#ifndef AMBER_TUI_RENDER_ENGINE_H
#define AMBER_TUI_RENDER_ENGINE_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "input_line_layout.h"
#include "markdown.h"
#include "rich.h"
#include "palette.h"
#include "status_bar_layout.h"

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"

namespace tui {
class Tui;
struct Window;

class RenderEngine {
public:
    explicit RenderEngine(Tui& tui);

    int height() const;
    int width() const;
    int chat_top() const;
    int chat_height() const;
    int lines_per_page() const;
    int max_scroll(const Window& w) const;
    int max_scroll() const;
    std::vector<rich::Line> build_view(const Window& w) const;
    std::vector<rich::Line> build_view_without_working(const Window& w) const;

    static size_t utf8_len(const std::string& s, size_t i);
    static std::vector<std::string> wrap_text(const std::string& text, int w);
    static void append_rich_to(std::vector<rich::Line>& view, const std::string& text, int color,
                               int w);
    void append_rich_to(Window& w, const rich::Line& l);

    void draw();
    void draw_status_bar(const std::string& tail);
    // Repaint the bar on the second. A segment that renders wall-clock time
    // (the clock plugin) is correct because of this cadence, not because it
    // schedules anything itself: segments are pure reads of the moment they
    // are painted.
    void tick_clock();
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    void draw_drawer(const std::string& input);

    static std::string drawer_token(const std::string& input);
    static bool drawer_has_arg(const std::string& input);
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
    };
    void request_git_refresh();
    std::shared_ptr<const GitState> git_state() const;
    md::Style& md_style() noexcept { return md_style_; }

    // UI-state accessors (state lives here; Tui facade forwards).
    bool dirty() const noexcept { return dirty_; }
    void mark_dirty() noexcept { dirty_ = true; }
    void clear_dirty() noexcept { dirty_ = false; }
    void flush() const { doupdate(); }
    std::chrono::steady_clock::time_point last_status_tick() const noexcept {
        return last_status_tick_;
    }
    void set_last_status_tick(std::chrono::steady_clock::time_point t) noexcept {
        last_status_tick_ = t;
    }
    bool drawer_open() const noexcept { return drawer_open_; }
    void set_drawer_open(bool v) noexcept { drawer_open_ = v; }
    int drawer_sel() const noexcept { return drawer_sel_; }
    void set_drawer_sel(int v) noexcept { drawer_sel_ = v; }
    bool scroll_mode() const noexcept { return scroll_mode_; }
    void set_scroll_mode(bool v) noexcept { scroll_mode_ = v; }
    bool show_reasoning() const noexcept { return show_reasoning_; }
    void set_show_reasoning(bool v) noexcept { show_reasoning_ = v; }
    bool working_visible() const noexcept { return working_visible_; }
    void mark_working() noexcept;
    void clear_working() noexcept { working_visible_ = false; }
    void advance_anim() noexcept { ++anim_phase_; }
    int anim_phase() const noexcept { return anim_phase_; }

private:
    // A status segment is the L1 layout's type: the bar is laid out purely
    // (status_bar_layout) and this class only paints the plan.
    using Seg = status_bar_layout::Segment;
    std::vector<Seg> bar_segments() const;
    agent::StatusSnapshot build_status_snapshot() const;
    static int tone_pair(agent::StatusTone tone);
    static int display_cols(const std::string& s);
    static std::wstring to_wide(const std::string& s);
    static std::string kfmt(long n);
    static int gauge_pair(double f);
    // Draw one input-line piece at `x`, advancing it (truncated to the width).
    void put_input(int y, int w, int& x, const std::string& text, int pair, int attrs = 0);
    // Draw the dim completion hint after the input, when the cursor is at the end.
    void draw_input_shadow(int y, int w, int prompt_w, int scroll_off, const std::string& input,
                           std::size_t cursor, const std::string& shadow);

    // The activity word leading the working indicator ("thinking", "talking",
    // "compressing", "searching", "working", ...) derived from the agent's
    // current run state, the in-flight tool, and whether compression is
    // running.
    std::string activity_verb() const;

    Tui& tui_;
    Canvas chat_canvas_;
    md::Style md_style_;
    // Worker-side: the only place that forks git. current_project_name() is a
    // getcwd (no fork), used to seed the prompt before the first refresh lands.
    static GitState read_git_state();
    static std::string current_project_name();

    struct GitPublication {
        std::shared_ptr<const GitState> state;
        std::atomic<bool> in_flight{false};
    };
    std::shared_ptr<GitPublication> git_pub_ = std::make_shared<GitPublication>();
    bool drawer_open_ = false;
    int drawer_sel_ = 0;
    bool scroll_mode_ = false;
    bool show_reasoning_ = true;
    int anim_phase_ = 0;
    std::chrono::steady_clock::time_point working_since_{};
    bool working_visible_ = false;
    bool dirty_ = true;
    std::chrono::steady_clock::time_point last_status_tick_{};
};

} // namespace tui

#endif