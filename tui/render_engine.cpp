#include "render_engine.h"
#include "tui.h"

namespace tui {

RenderEngine::RenderEngine(Tui& tui) : tui_(tui) {}

void RenderEngine::draw() { tui_.draw(); }
void RenderEngine::draw_status_bar(const std::string& tail) { tui_.draw_status_bar(tail); }
void RenderEngine::tick_clock() { tui_.tick_clock(); }
void RenderEngine::draw_input(const std::string& s, size_t cursor, const std::string& shadow) { tui_.draw_input(s, cursor, shadow); }
void RenderEngine::draw_drawer(const std::string& input) { tui_.draw_drawer(input); }
void RenderEngine::advance_tool_spinners() { tui_.advance_tool_spinners(); }
std::vector<rich::Line> RenderEngine::build_view(const Window& w) const { return tui_.build_view(w); }
std::vector<rich::Line> RenderEngine::build_view_without_working(const Window& w) const { return tui_.build_view_without_working(w); }
int RenderEngine::max_scroll(const Window& w) const { return tui_.max_scroll(w); }
int RenderEngine::max_scroll() const { return tui_.max_scroll(); }

std::vector<RenderEngine::Seg> RenderEngine::bar_segments() const {
    auto segs = tui_.bar_segments();
    std::vector<Seg> out;
    out.reserve(segs.size());
    for (auto& s : segs) out.push_back({s.text, s.pair, s.drop});
    return out;
}
int RenderEngine::display_cols(const std::string& s) { return Tui::display_cols(s); }
std::wstring RenderEngine::to_wide(const std::string& s) { return Tui::to_wide(s); }
std::string RenderEngine::kfmt(long n) { return Tui::kfmt(n); }
int RenderEngine::gauge_pair(double f) { return Tui::gauge_pair(f); }
int RenderEngine::height() const { return tui_.height(); }
int RenderEngine::width() const { return tui_.width(); }
int RenderEngine::chat_top() const { return tui_.chat_top(); }
int RenderEngine::chat_height() const { return tui_.chat_height(); }
int RenderEngine::lines_per_page() const { return tui_.lines_per_page(); }

} // namespace tui
