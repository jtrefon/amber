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

int option_width(const DialogOption& option) {
    return cols(option.key) + 2 + cols(option.label);
}

int footer_width(const std::vector<FooterSegment>& keys) {
    int width = 0;
    for (const auto& key : keys)
        width += cols(key.key) + cols(key.label) + 4; // [key] label
    if (keys.size() > 1)
        width += static_cast<int>(keys.size() - 1) * 2;
    return width;
}

std::string truncated_to_columns(const std::string& text, int max_columns, const char* ellipsis) {
    if (cols(text) <= max_columns)
        return text;
    const int room = max_columns - cols(ellipsis);
    if (room <= 0)
        return max_columns > 0 ? std::string(ellipsis) : std::string();
    std::string out;
    int used = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t len = text::utf8_len(text, i);
        const int width = text_columns(text.substr(i, len));
        if (used + width > room)
            break;
        out += text.substr(i, len);
        used += width;
        i += len;
    }
    return out + ellipsis;
}

int dialog_width(const DialogView& view, int screen_w) {
    int width = std::max(40, cols(view.title) + 6);
    width = std::max(width, widest(view.lines) + 6);
    width = std::max(width, widest(view.items) + 6);
    for (const auto& option : view.options)
        width = std::max(width, option_width(option) + 6);
    width = std::accumulate(
        view.fields.begin(), view.fields.end(), width, [](int current, const FieldSpec& field) {
            return std::max(current, cols(field.label) + cols(field.value) + 10);
        });
    width = std::max(width, button_row_width(view.buttons) + 6);
    width = std::max(width, view.footer_keys.empty() ? cols(view.footer) + 4
                                                     : footer_width(view.footer_keys) + 4);
    width = std::max(width, cols(view.notice) + 4);
    width = std::max(width, cols(view.status) + 4);
    width = std::max(width, view.preferred_width);
    const int cap = std::min(screen_w - 2, screen_w * 4 / 5);
    const int max_width = std::max(1, std::min(cap, screen_w - 2));
    const int min_width = std::min(40, max_width);
    return std::clamp(width, min_width, max_width);
}

int content_height(const DialogView& view, int lines) {
    int rows = lines;
    if (!view.options.empty())
        rows += 1 + static_cast<int>(view.options.size()); // blank + options
    if (!view.items.empty())
        rows += static_cast<int>(view.items.size()) + 1; // filter strip
    rows += static_cast<int>(view.fields.size());
    if (!view.notice.empty())
        ++rows;
    if (!view.status.empty())
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
    frame.lines = view.wrap_lines ? wrapped(view.lines, std::max(1, frame.width - 4)) : view.lines;
    const int rows = content_height(view, static_cast<int>(frame.lines.size()));
    frame.height = std::clamp(rows + 2, 5, std::max(5, screen.h - 1));
    return frame;
}

void paint_backdrop(Surface& surface, const Frame& frame, const Glyphs& glyphs) {
    surface.clear(Cell{std::string(glyphs.shadow), Style{Role::Shadow}});
    surface.fill({0, 0, frame.width, frame.height}, Cell{" ", Style{Role::DialogSurface}});
    surface.draw_box({0, 0, frame.width, frame.height}, glyphs, Style{Role::DialogFrame});
}

void paint_title(Surface& surface, const DialogView& view, const Frame& frame,
                 const Glyphs& glyphs) {
    if (view.title.empty())
        return;
    std::string title = truncated_to_columns(view.title, frame.width - 8, text::glyph::ellipsis());
    title = " " + title + " ";
    const int x = std::max(2, (frame.width - cols(title)) / 2);
    surface.put_text(x - 1, 0, glyphs.tee_right, Style{Role::DialogFrame});
    surface.put_text(x, 0, title, Style{Role::DialogTitle});
    surface.put_text(x + cols(title), 0, glyphs.tee_left, Style{Role::DialogFrame});
}

void paint_lines(Surface& surface, const DialogView& view, const Frame& frame, int& y, int bottom) {
    int index = 0;
    for (const auto& line : frame.lines) {
        if (y >= bottom)
            return;
        const bool focused = view.focus_line == index;
        const Style style{focused ? Role::DialogSelected : Role::DialogText};
        if (focused)
            surface.fill({1, y, frame.width - 2, 1}, Cell{" ", style});
        surface.put_text(2, y, line, style);
        ++y;
        ++index;
    }
}

void paint_options(Surface& surface, const DialogView& view, const Frame& frame, int& y,
                   int bottom) {
    if (view.options.empty())
        return;
    if (!frame.lines.empty() && y < bottom)
        ++y; // one blank row between the summary and the choices
    for (std::size_t i = 0; i < view.options.size() && y < bottom; ++i) {
        const bool selected = static_cast<int>(i) == view.selected_option;
        const Style row{selected ? Role::DialogSelected : Role::DialogText};
        if (selected)
            surface.fill({1, y, frame.width - 2, 1}, Cell{" ", row});
        const DialogOption& option = view.options[i];
        surface.put_text(2, y, option.key,
                         selected ? Style{Role::DialogSelected} : Style{Role::DialogAccent});
        surface.put_text(2 + cols(option.key) + 2, y, option.label, row);
        ++y;
    }
}

void paint_items(Surface& surface, const DialogView& view, const Frame& frame, int& y, int bottom,
                 const Glyphs& glyphs) {
    if (view.items.empty())
        return;
    const int right = frame.width - 2;
    const int shown = std::min(static_cast<int>(view.items.size()) - view.first_item,
                               std::max(0, bottom - y - 1));
    for (int i = 0; i < shown; ++i) {
        const int item = view.first_item + i;
        const Style style{item == view.selected_item ? Role::DialogSelected : Role::DialogText};
        if (item == view.selected_item)
            surface.fill({1, y, frame.width - 2, 1}, Cell{" ", style});
        surface.put_text(2, y, view.items[static_cast<std::size_t>(item)], style);
        ++y;
    }
    if (y < bottom) {
        const std::string filter = view.filter.empty() ? "/" : "/ " + view.filter;
        surface.put_text(2, y, filter, Style{Role::DialogMuted});
        ++y;
    }
    if (view.scroll_up)
        surface.put_text(right - 1, 1, glyphs.arrow_up, Style{Role::DialogMuted});
    if (view.scroll_down)
        surface.put_text(right - 1, std::max(1, bottom - 2), glyphs.arrow_down,
                         Style{Role::DialogMuted});
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
        surface.put_text(2, y, label, Style{Role::DialogMuted});
        const int value_x = 2 + cols(label);
        const int value_w = std::max(1, right - value_x);
        if (focused)
            surface.fill({value_x, y, value_w, 1}, Cell{" ", Style{Role::DialogSelected}});
        surface.put_text(value_x, y, field.value,
                         focused ? Style{Role::DialogSelected} : Style{Role::DialogText});
        if (focused) {
            const std::size_t at = std::min(view.field_cursor, field.value.size());
            cursor.x = std::min(value_x + text_columns(field.value.substr(0, at)), right - 1);
            cursor.y = y;
        }
        ++y;
    }
}

void paint_notice(Surface& surface, const DialogView& view, const Frame& frame, int& y,
                  int bottom) {
    if (view.notice.empty() || y >= bottom)
        return;
    surface.fill({1, y, frame.width - 2, 1}, Cell{" ", Style{Role::DialogError}});
    surface.put_text(2, y, view.notice, Style{Role::DialogError});
    ++y;
}

void paint_status(Surface& surface, const DialogView& view, const Frame& frame, int y) {
    if (view.status.empty())
        return;
    const int x = std::max(2, frame.width - 2 - cols(view.status));
    surface.put_text(x, y, view.status, Style{Role::DialogMuted});
}

void paint_buttons(Surface& surface, const DialogView& view, const Frame& frame, int y) {
    if (view.buttons.empty())
        return;
    int x = std::max(1, (frame.width - button_row_width(view.buttons)) / 2);
    for (std::size_t i = 0; i < view.buttons.size(); ++i) {
        const Style style = static_cast<int>(i) == view.selected_button
                                ? Style{Role::DialogButtonFocus}
                                : Style{Role::DialogButton};
        surface.fill({x, y, cols(view.buttons[i]), 1}, Cell{" ", style});
        surface.put_text(x, y, view.buttons[i], style);
        x += cols(view.buttons[i]) + 2;
    }
}

void paint_footer(Surface& surface, const DialogView& view, const Frame& frame, int y,
                  const Glyphs& glyphs) {
    surface.fill({0, y, frame.width, 1}, Cell{" ", Style{Role::DialogSurface}});
    surface.put_text(0, y, glyphs.bottom_left, Style{Role::DialogFrame});
    surface.put_text(frame.width - 1, y, glyphs.bottom_right, Style{Role::DialogFrame});

    if (!view.footer_keys.empty()) {
        std::vector<FooterSegment> keys = view.footer_keys;
        while (keys.size() > 1 && footer_width(keys) > frame.width - 4)
            keys.erase(keys.begin());
        surface.put_text(1, y, glyphs.hline, Style{Role::DialogFrame});
        int x = 3;
        for (const auto& key : keys) {
            const std::string chip = "[" + key.key + "]";
            if (x + cols(chip) + cols(key.label) >= frame.width - 1)
                break;
            surface.put_text(x, y, chip, Style{Role::DialogFooterKey});
            x += cols(chip) + 1;
            surface.put_text(x, y, key.label, Style{Role::DialogFooterLabel});
            x += cols(key.label) + 2;
        }
        for (; x < frame.width - 1; ++x)
            surface.put_text(x, y, glyphs.hline, Style{Role::DialogFrame});
        return;
    }

    if (!view.footer.empty()) {
        const std::string footer =
            truncated_to_columns(view.footer, frame.width - 4, text::glyph::ellipsis());
        const int start = centered(frame.width, footer);
        surface.put_text(start, y, footer, Style{Role::DialogFooterLabel});
        for (int x = 1; x < start; ++x)
            surface.put_text(x, y, glyphs.hline, Style{Role::DialogFrame});
        for (int x = start + cols(footer); x < frame.width - 1; ++x)
            surface.put_text(x, y, glyphs.hline, Style{Role::DialogFrame});
    } else {
        surface.draw_hline(1, y, frame.width - 2, glyphs.hline, Style{Role::DialogFrame});
    }
}

} // namespace

PaintedDialog paint_dialog(const DialogView& view, Size screen) {
    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    const Frame frame = frame_for(view, screen);
    Surface surface(frame.width + 1, frame.height + 1);
    paint_backdrop(surface, frame, glyphs);
    paint_title(surface, view, frame, glyphs);

    const int body_last = frame.height - 2;
    // Buttons own the last body row and keep a blank row above them. A status
    // line owns the last body row itself; everything else uses the row too.
    const int content_end = !view.buttons.empty() ? body_last - 1 : body_last + 1;
    const int status_row = view.buttons.empty() ? body_last : body_last - 1;
    int y = 1;
    Cursor cursor;
    paint_lines(surface, view, frame, y, content_end);
    paint_options(surface, view, frame, y, content_end);
    paint_items(surface, view, frame, y, content_end, glyphs);
    paint_fields(surface, view, frame, y, content_end, cursor);
    paint_notice(surface, view, frame, y, content_end);
    if (!view.status.empty())
        paint_status(surface, view, frame, status_row);
    if (!view.buttons.empty())
        paint_buttons(surface, view, frame, body_last);
    paint_footer(surface, view, frame, frame.height - 1, glyphs);

    return PaintedDialog{std::move(surface), std::max(0, (screen.w - frame.width - 1) / 2),
                         std::max(0, (screen.h - frame.height - 1) / 2), cursor.x, cursor.y};
}

} // namespace tui
