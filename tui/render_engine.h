#ifndef AMBER_TUI_RENDER_ENGINE_H
#define AMBER_TUI_RENDER_ENGINE_H

#include <string>
#include <vector>
#include <chrono>

#include "canvas.h"
#include "markdown.h"
#include "rich.h"
#include "palette.h"

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
    static void append_rich_to(std::vector<rich::Line>& view,
                               const std::string& text, int color, int w);
    void append_rich_to(Window& w, const rich::Line& l);

    void draw();
    void draw_status_bar(const std::string& tail);
    void tick_clock();
    void draw_input(const std::string& s, size_t cursor = 0, const std::string& shadow = "");
    void draw_drawer(const std::string& input);

    static std::string drawer_token(const std::string& input);
    static bool drawer_has_arg(const std::string& input);
    std::vector<const palette::Command*> filter_commands(const std::string& token);

    void git_refresh();
    md::Style& md_style() noexcept { return md_style_; }
    const std::string& git_project() const noexcept { return git_project_; }
    const std::string& git_branch() const noexcept { return git_branch_; }
    int git_ins() const noexcept { return git_ins_; }
    int git_del() const noexcept { return git_del_; }

    // UI-state accessors (state lives here; Tui facade forwards).
    bool dirty() const noexcept { return dirty_; }
    void mark_dirty() noexcept { dirty_ = true; }
    void clear_dirty() noexcept { dirty_ = false; }
    void flush() const { doupdate(); }
    std::chrono::steady_clock::time_point last_status_tick() const noexcept { return last_status_tick_; }
    void set_last_status_tick(std::chrono::steady_clock::time_point t) noexcept { last_status_tick_ = t; }
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
    struct Seg {
        std::string text;
        int pair;
        int drop;
    };
    std::vector<Seg> bar_segments() const;
    static int display_cols(const std::string& s);
    static std::wstring to_wide(const std::string& s);
    static std::string kfmt(long n);
    static int gauge_pair(double f);

    Tui& tui_;
    Canvas chat_canvas_;
    md::Style md_style_;
    std::string git_project_;
    std::string git_branch_;
    int git_ins_ = 0;
    int git_del_ = 0;
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