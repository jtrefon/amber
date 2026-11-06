#include "tui/dialog_view.h"

#include "tui/glyphs.h"
#include "tui/textutil.h"

#include <algorithm>
#include <numeric>
#include <string>

namespace tui {

namespace {

int cols(const std::string& text) {
    return text_columns(text);
}

int centered(int width, const std::string& text) {
    return std::max(0, (width - cols(text)) / 2);
}

std::vector<std::string> wrapped(const std::vector<std::string>& lines, int width) {
    std::vector<std::string> out;
    for (const auto& line : lines) {
        const auto pieces = text::wrap(line, width);
        if (pieces.empty())
            out.emplace_back();
        else
            out.insert(out.end(), pieces.begin(), pieces.end());
    }
    return out;
}

int widest(const std::vector<std::string>& rows) {
    return std::accumulate(rows.begin(), rows.end(), 0, [](int width, const std::string& row) {
        return std::max(width, cols(row));
    });
}

int button_row_width(const std::vector<std::string>& buttons) {
    return std::accumulate(
        buttons.begin(), buttons.end(), 0,
        [](int width, const std::string& button) { return width + cols(button) + 2; });
}

int dialog_width(const DialogView& view, int screen_w) {
    int width = std::max(50, cols(view.title) + 6);
    width = std::max(width, widest(view.lines) + 6);
    width = std::max(width, widest(view.items) + 6);
    width = std::accumulate(
        view.fields.begin(), view.fields.end(), width, [](int current, const FieldSpec& field) {
            return std::max(current, cols(field.label) + cols(field.value) + 10);
        });
    width = std::max(width, button_row_width(view.buttons) + 6);
    width = std::max(width, cols(view.footer) + 4);
    width = std::max(width, cols(view.notice) + 4);
    return std::clamp(width, 24, std::max(24, screen_w - 2));
}

int content_height(const DialogView& view, int lines) {
    int rows = lines;
    if (!view.items.empty())
        rows += static_cast<int>(view.items.size()) + 1; // filter strip
    rows += static_cast<int>(view.fields.size());
    if (!view.notice.empty())
        ++rows;
    if (!view.buttons.empty())
        rows += 2; // blank + buttons
    return rows;
}

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::string> lines;
};

Frame frame_for(const DialogView& view, Size screen) {
    Frame frame;
    frame.width = dialog_width(view, screen.w);
    frame.lines = wrapped(view.lines, std::max(1, frame.width - 4));
    const int rows = content_height(view, static_cast<int>(frame.lines.size()));
    frame.height = std::clamp(rows + 3, 6, std::max(6, screen.h - 1));
    return frame;
}

void paint_backdrop(Surface& surface, const Frame& frame) {
    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    surface.clear(Cell{" ", Style{Role::Shadow}});
    surface.fill({0, 0, frame.width, frame.height}, Cell{" ", Style{Role::SurfaceAlt}});
    surface.draw_box({0, 0, frame.width, frame.height}, glyphs, Style{Role::Frame});
}

void paint_title(Surface& surface, const DialogView& view, const Frame& frame) {
    surface.put_text(centered(frame.width, view.title), 0, view.title, Style{Role::Title});
}

void paint_lines(Surface& surface, const Frame& frame, int& y, int bottom) {
    for (const auto& line : frame.lines) {
        if (y >= bottom)
            return;
        surface.put_text(2, y, line, Style{Role::Text});
        ++y;
    }
}

void paint_items(Surface& surface, const DialogView& view, const Frame& frame, int& y, int bottom) {
    if (view.items.empty())
        return;
    const int right = frame.width - 2;
    const int shown = std::min(static_cast<int>(view.items.size()) - view.first_item,
                               std::max(0, bottom - y - 1));
    for (int i = 0; i < shown; ++i) {
        const int item = view.first_item + i;
        const Style style = item == view.selected_item ? Style{Role::Selected} : Style{Role::Text};
        if (item == view.selected_item)
            surface.fill({2, y, right - 2, 1}, Cell{" ", style});
        surface.put_text(2, y, view.items[static_cast<std::size_t>(item)], style);
        ++y;
    }
    if (y < bottom) {
        const std::string filter = view.filter.empty() ? "/" : "/ " + view.filter;
        surface.put_text(2, y, filter, Style{Role::Muted});
        ++y;
    }
    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    if (view.scroll_up)
        surface.put_text(right - 1, 1, glyphs.arrow_up, Style{Role::Muted});
    if (view.scroll_down)
        surface.put_text(right - 1, std::max(1, bottom - 2), glyphs.arrow_down, Style{Role::Muted});
}

struct Cursor {
    int x = -1;
    int y = -1;
};

void paint_fields(Surface& surface, const DialogView& view, const Frame& frame, int& y, int bottom,
                  Cursor& cursor) {
    const int right = frame.width - 2;
    for (std::size_t i = 0; i < view.fields.size() && y < bottom; ++i) {
        const FieldSpec& field = view.fields[i];
        const bool focused = static_cast<int>(i) == view.focused_field;
        const std::string label = field.label + ": ";
        surface.put_text(2, y, label, Style{Role::Muted});
        const int value_x = 2 + cols(label);
        const int value_w = std::max(1, right - value_x);
        if (focused)
            surface.fill({value_x, y, value_w, 1}, Cell{" ", Style{Role::Selected}});
        surface.put_text(value_x, y, field.value,
                         focused ? Style{Role::Selected} : Style{Role::Text});
        if (focused) {
            const std::size_t at = std::min(view.field_cursor, field.value.size());
            cursor.x = std::min(value_x + text_columns(field.value.substr(0, at)), right - 1);
            cursor.y = y;
        }
        ++y;
    }
}

void paint_notice(Surface& surface, const DialogView& view, int& y, int bottom) {
    if (view.notice.empty() || y >= bottom)
        return;
    surface.put_text(2, y, view.notice, Style{Role::Error});
    ++y;
}

void paint_buttons(Surface& surface, const DialogView& view, const Frame& frame, int bottom) {
    if (view.buttons.empty())
        return;
    int x = std::max(0, (frame.width - button_row_width(view.buttons)) / 2);
    for (std::size_t i = 0; i < view.buttons.size(); ++i) {
        const Style style =
            static_cast<int>(i) == view.selected_button ? Style{Role::Selected} : Style{Role::Text};
        surface.put_text(x, bottom - 1, view.buttons[i], style);
        x += cols(view.buttons[i]) + 2;
    }
}

void paint_footer(Surface& surface, const DialogView& view, const Frame& frame) {
    if (!view.footer.empty())
        surface.put_text(centered(frame.width, view.footer), frame.height - 2, view.footer,
                         Style{Role::Muted});
}

} // namespace

PaintedDialog paint_dialog(const DialogView& view, Size screen) {
    const Frame frame = frame_for(view, screen);
    Surface surface(frame.width + 1, frame.height + 1);
    paint_backdrop(surface, frame);
    paint_title(surface, view, frame);

    const int bottom = frame.height - 2; // content stops before the footer row
    int y = 1;
    Cursor cursor;
    paint_lines(surface, frame, y, bottom);
    paint_items(surface, view, frame, y, bottom);
    paint_fields(surface, view, frame, y, bottom, cursor);
    paint_notice(surface, view, y, bottom);
    paint_buttons(surface, view, frame, bottom);
    paint_footer(surface, view, frame);

    return PaintedDialog{std::move(surface), std::max(0, (screen.w - frame.width - 1) / 2),
                         std::max(0, (screen.h - frame.height - 1) / 2), cursor.x, cursor.y};
}

} // namespace tui
