#ifndef AMBER_TUI_RENDER_ENGINE_H
#define AMBER_TUI_RENDER_ENGINE_H

#include <string>
#include <vector>
#include <chrono>

namespace tui {
class Tui;
struct Window;
namespace rich { struct Line; }

class RenderEngine {
public:
    explicit RenderEngine(Tui& tui);
    void draw();
    void draw_status_bar(const std::string& tail);
    void tick_clock();
    void draw_input(const std::string& s, size_t cursor, const std::string& shadow);
    void draw_drawer(const std::string& input);
    void advance_tool_spinners();
    std::vector<rich::Line> build_view(const Window& w) const;
    std::vector<rich::Line> build_view_without_working(const Window& w) const;
    int max_scroll(const Window& w) const;
    int max_scroll() const;

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
    int height() const;
    int width() const;
    int chat_top() const;
    int chat_height() const;
    int lines_per_page() const;

    Tui& tui_;
};

} // namespace tui

#endif
