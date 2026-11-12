// TextCanvas is the chat scrollback on the cell grid. The cache is the point:
// an unchanged view or a scroll must not re-wrap, and a width change must wrap
// exactly once even though the renderer resizes and then supplies lines.

#include "test_util.h"
#include "tui/text_canvas.h"

#include <chrono>
#include <string>
#include <vector>

namespace {

tui::rich::Line text_line(const std::string& text) {
    tui::rich::Line line;
    line.runs.push_back(tui::rich::Run{text, tui::P_ASSISTANT, false, false, false, false});
    return line;
}

std::vector<tui::rich::Line> numbered_lines(int count) {
    std::vector<tui::rich::Line> lines;
    lines.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        lines.push_back(text_line("line " + std::to_string(i)));
    return lines;
}

} // namespace

TEST(text_canvas_wraps_once_and_reuses_the_wrap_for_identical_lines) {
    tui::TextCanvas canvas;
    canvas.set_viewport(5, 20);
    const auto lines = numbered_lines(50);
    canvas.set_lines(lines);
    ASSERT_EQ(canvas.wrapped_count(), 50);
    ASSERT_EQ(canvas.wrap_count(), 1u);

    canvas.set_top(10);
    ASSERT_EQ(canvas.top(), 10);
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 1u);

    // The renderer rebuilds and re-supplies the same view every frame.
    canvas.set_lines(lines);
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 1u);
}

TEST(text_canvas_rewraps_once_on_a_width_change_and_not_on_height) {
    tui::TextCanvas canvas;
    canvas.set_viewport(4, 20);
    canvas.set_lines(numbered_lines(30));
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 1u);

    canvas.set_viewport(4, 10); // width change: dirty, but no wrap yet
    ASSERT_EQ(canvas.wrap_count(), 1u);
    canvas.set_lines(numbered_lines(30)); // the same view after the resize
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 2u);

    canvas.set_viewport(6, 10); // height only: no rewrap
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 2u);
}

TEST(text_canvas_new_content_marks_the_wrap_dirty) {
    tui::TextCanvas canvas;
    canvas.set_viewport(4, 20);
    canvas.set_lines(numbered_lines(5));
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 1u);

    canvas.set_lines(numbered_lines(6));
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 2u);
}

TEST(text_canvas_paints_the_viewport_into_a_surface) {
    tui::TextCanvas canvas;
    canvas.set_viewport(2, 10);
    std::vector<tui::rich::Line> lines = {text_line("alpha"), text_line("beta")};
    lines.push_back(tui::rich::Line{});
    lines.back().is_hr = true;
    canvas.set_lines(std::move(lines));
    canvas.set_top(1);

    const tui::Surface& surface = canvas.surface();
    ASSERT_EQ(surface.width(), 10);
    ASSERT_EQ(surface.height(), 2);
    ASSERT(surface.row_text(0).find("beta") != std::string::npos);
    ASSERT(surface.row_text(1).find("\u2500") != std::string::npos ||
           surface.row_text(1).find("-") != std::string::npos);
}

TEST(text_canvas_wide_glyphs_keep_their_continuation_cell) {
    tui::TextCanvas canvas;
    canvas.set_viewport(1, 6);
    canvas.set_lines({text_line("\u754c\u754c")});
    const tui::Surface& surface = canvas.surface();
    ASSERT_EQ(surface.at(0, 0).glyph, std::string("\u754c"));
    ASSERT(surface.at(1, 0).continuation);
    ASSERT_EQ(surface.at(2, 0).glyph, std::string("\u754c"));
    ASSERT(surface.at(3, 0).continuation);
}

TEST(text_canvas_ten_thousand_lines_scroll_without_rewrapping) {
    tui::TextCanvas canvas;
    canvas.set_viewport(20, 120);
    canvas.set_lines(numbered_lines(10000));
    canvas.surface();
    ASSERT_EQ(canvas.wrap_count(), 1u);
    ASSERT_EQ(canvas.wrapped_count(), 10000);
    ASSERT_EQ(canvas.max_top(), 9980);

    for (const int top : {0, 5000, 9999}) {
        canvas.set_top(top);
        canvas.surface();
        ASSERT_EQ(canvas.wrap_count(), 1u);
        ASSERT_EQ(canvas.top(), std::min(top, canvas.max_top()));
    }
}

// The acceptance scenario: 10k rich lines, a 120x40 terminal, 120 scroll
// frames. Only painting the viewport is allowed; a rewrap per frame would be
// orders of magnitude slower. The bound is deliberately generous so a coverage
// or sanitizer build cannot make it flaky.
TEST(text_canvas_ten_thousand_line_scroll_stays_in_the_frame_budget) {
    tui::TextCanvas canvas;
    canvas.set_viewport(40, 120);
    canvas.set_lines(numbered_lines(10000));
    canvas.surface();

    // The renderer rebuilds the view and re-supplies it every frame; the
    // equality check is part of the budget, not just the scroll.
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        canvas.set_lines(numbered_lines(10000));
        canvas.set_top((frame * 83) % 9960);
        canvas.surface();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT(elapsed.count() < 2000); // flake-allow: 120 frames over 10k lines is the acceptance
                                    // budget; measured well under 1 s even instrumented
    ASSERT_EQ(canvas.wrap_count(), 1u);
}
