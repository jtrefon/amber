#include "tui.h"
#include "tool_display.h"

#include <ctime>
#include <string>
#include <vector>
#include <utility>

namespace tui {

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
    append_line_to(win(), color, text, ts);
}
size_t Tui::append_line_to(Window& w, int color, const std::string& text) {
    return append_line_to(w, color, text, timestamp());
}
size_t Tui::append_line_to(Window& w, int color, const std::string& text,
                           const std::string& ts) {
    // Build one RichLine with a dim timestamp run followed by the body run,
    // then wrap it to the current width so wrapped continuations align.
    rich::Line head;
    if (!ts.empty())
        head.runs.push_back({ts, P_REASONING, false, true});  // faint timestamp
    rich::Run body; body.pair = color; body.text = text;
    head.runs.push_back(body);
    auto wrapped = rich::wrap(head, render_engine_->width());
    size_t first = w.lines.size();
    for (auto& l : wrapped) w.lines.push_back(std::move(l));
    trim_lines(w);
    int max = render_engine_->max_scroll(w);
    if (w.scroll_top >= max - 2)
        w.scroll_top = max;
    return first;
}
void Tui::append_rich(const rich::Line& l) {
    append_rich_to(win(), l);
}

void Tui::append_rich_to(Window& w, const rich::Line& l) {
    w.lines.push_back(l);
    trim_lines(w);
    w.scroll_top = render_engine_->max_scroll(w);
}
void Tui::append_markdown(Window& w, const std::string& md) {
    if (w.markdown_on) {
        // Render the assistant reply as Markdown, then prepend a faint
        // timestamp run to the first rendered line so it stays on the same
        // line as the reply (matching user/tool/status lines) instead of
        // floating on its own row above a blank gap.
        auto lines = md::render(md, render_engine_->md_style());
        if (!lines.empty()) {
            rich::Run ts;
            ts.text = (w.stream_ts.empty() ? timestamp()
                                           : w.stream_ts) + " ";
            ts.pair = P_REASONING;
            ts.dim = true;
            lines.front().runs.insert(lines.front().runs.begin(),
                                      std::move(ts));
            for (auto& l : lines) w.lines.push_back(std::move(l));
        }
    } else {
        append_line_to(w, P_ASSISTANT, md);
    }
    trim_lines(w);
    w.scroll_top = render_engine_->max_scroll(w);
}
void Tui::banner(const std::string& text) {
    rich::Line l;
    rich::Run r; r.pair = P_BANNER; r.bold = true; r.text = text;
    l.runs.push_back(r);
    win().lines.push_back(std::move(l));
    win().scroll_top = render_engine_->max_scroll();
}
void Tui::trim_lines(Window& w) {
    if (w.lines.size() <= 10000) return;
    w.lines.erase(w.lines.begin(), w.lines.begin() + 5000);
    // Pending tool lines hold indices into this window's scrollback; shift
    // surviving entries by the trimmed amount and invalidate the rest so
    // spinner animation can never write to a moved line.
    for (auto& pt : router_->pending_tools()) {
        if (pt.window_id != w.id) continue;
        if (pt.index < 5000) {
            pt.index = std::string::npos;
        } else {
            pt.index -= 5000;
        }
    }
}

std::string RenderEngine::drawer_token(const std::string& input) {
    return palette::token(input);
}
bool RenderEngine::drawer_has_arg(const std::string& input) {
    return palette::has_arg(input);
}
std::vector<const palette::Command*> RenderEngine::filter_commands(const std::string& token) {
    return palette::filter(tui_.commands(), token);
}


} // namespace tui
