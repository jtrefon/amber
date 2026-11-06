#include "tui/modal_host.h"

#include "tui/approval_model.h"
#include "tui/bracket_paste.h"
#include "tui/keys.h"
#include "tui/list_state.h"
#include "tui/terminal.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tui {

namespace {

int list_rows(Size screen) {
    return std::clamp(screen.h - 8, 3, 12);
}

DialogKey map_choice(ListState& list, int ch, int visible) {
    const ListState::Action action = list.key(ch, visible);
    if (action == ListState::Action::Select)
        return DialogKey::Accept;
    if (action == ListState::Action::Cancel)
        return DialogKey::Cancel;
    return action == ListState::Action::Redraw ? DialogKey::Redraw : DialogKey::None;
}

std::string approval_label(int index, int remaining) {
    static const char* const kLabels[] = {"[1] Allow once", "[2] Allow session", "[3] Always allow",
                                          "[4] Always deny"};
    if (index < 0 || index > 3)
        return {};
    std::string label = kLabels[index];
    if (remaining > 0)
        label += " (" + std::to_string(remaining) + "s)";
    return label;
}

std::vector<std::string> approval_buttons(int remaining) {
    std::vector<std::string> buttons;
    buttons.reserve(4);
    for (int i = 0; i < 4; ++i)
        buttons.push_back(approval_label(i, remaining));
    return buttons;
}

} // namespace

bool ModalHost::confirm(const std::string& title, const std::string& message) {
    ConfirmState state;
    DialogView view;
    view.title = title;
    view.lines = {message};
    view.buttons = {"[ Yes ]", "[ No ]"};
    view.selected_button = 1; // the safe choice starts focused
    view.footer = "Tab/left/right: switch   Enter: confirm   Esc: cancel";

    const Outcome outcome = run(
        view,
        [&](int ch) {
            const DialogKey key = state.key(ch);
            if (key == DialogKey::Accept || key == DialogKey::Cancel) {
                view.selected_button = state.yes() ? 0 : 1;
                return key == DialogKey::Accept ? Outcome::Accept : Outcome::Cancel;
            }
            if (key == DialogKey::Redraw)
                view.selected_button = state.yes() ? 0 : 1;
            return Outcome::Continue;
        },
        -1);
    return outcome == Outcome::Accept && state.yes();
}

int ModalHost::choose(const std::string& title, const std::vector<std::string>& items) {
    ListState list(items);
    const int visible = list_rows(Terminal{}.size());
    DialogView view;
    view.title = title;
    view.footer = "Up/Down: move   Enter: select   /: filter   Esc: cancel";
    view.items = list.filtered();
    view.selected_item = list.selection();

    const Outcome outcome = run(
        view,
        [&](int ch) {
            const DialogKey key = map_choice(list, ch, visible);
            view.items = list.filtered();
            view.selected_item = list.selection();
            view.first_item = list.scroll_offset();
            view.filter = list.filter();
            view.filter_mode = list.filter_mode();
            if (key == DialogKey::Accept) {
                list.remap_selection();
                return Outcome::Accept;
            }
            return key == DialogKey::Cancel ? Outcome::Cancel : Outcome::Continue;
        },
        -1);
    return outcome == Outcome::Accept ? list.selection() : -1;
}

void ModalHost::info(const std::string& title, const std::vector<std::string>& rows) {
    InfoState state;
    const int visible = std::clamp(Terminal{}.size().h - 8, 3, 20);
    DialogView view;
    view.title = title;
    view.footer = "Up/Down: scroll   Enter/Esc: close";
    const auto fill = [&] {
        const int total = static_cast<int>(rows.size());
        const int top = std::min(state.top(), std::max(0, total - visible));
        view.lines.assign(rows.begin() + top, rows.begin() + std::min(total, top + visible));
        view.scroll_up = top > 0;
        view.scroll_down = top + visible < total;
    };
    fill();
    run(
        view,
        [&](int ch) {
            const DialogKey key = state.key(ch, visible, static_cast<int>(rows.size()));
            fill();
            if (key == DialogKey::Accept)
                return Outcome::Accept;
            return key == DialogKey::Cancel ? Outcome::Cancel : Outcome::Continue;
        },
        -1);
}

bool ModalHost::handle_paste(FormState& state) {
    Terminal terminal;
    std::vector<int> burst;
    int next = terminal.read_key_now();
    while (next != keys::kNone) {
        burst.push_back(next);
        next = terminal.read_key_now();
    }
    if (burst.empty())
        return false; // a lone Esc is the cancel key

    BracketPasteDecoder decoder;
    char ignored = 0;
    decoder.feed(27, ignored);
    const PasteBurst start = consume_paste_burst(decoder, burst);
    if (!start.started)
        return false;
    std::string text(start.insert.begin(), start.insert.end());
    while (decoder.pasting()) {
        const int key = terminal.read_key();
        if (key == keys::kNone)
            break;
        char out = 0;
        const BracketPasteDecoder::Event event = decoder.feed(key, out);
        if (event == BracketPasteDecoder::Event::Text)
            text.push_back(out);
        if (event == BracketPasteDecoder::Event::End)
            break;
    }
    state.insert_text(text);
    return true;
}

bool ModalHost::form(const std::string& title, std::vector<FieldSpec>& fields,
                     const std::string& notice) {
    FormState state(fields, notice);
    DialogView view;
    view.title = title;
    view.notice = notice;
    view.buttons = {"[  OK  ]", "[ Cancel ]"};
    view.footer = "Tab: next field   Enter: confirm   Esc: cancel";
    const auto fill = [&] {
        view.fields = state.fields();
        view.focused_field = state.on_field() ? static_cast<int>(state.field_index()) : -1;
        view.field_cursor = state.cursor();
        view.selected_button = state.ok_focused() ? 0 : (state.cancel_focused() ? 1 : -1);
    };
    fill();

    const Outcome outcome = run(
        view,
        [&](int ch) {
            if (ch == 27) {
                if (handle_paste(state)) {
                    fill();
                    return Outcome::Continue;
                }
                return Outcome::Cancel;
            }
            const DialogKey key = state.key(ch);
            fill();
            if (key == DialogKey::Accept)
                return state.accepted() ? Outcome::Accept : Outcome::Cancel;
            if (key == DialogKey::Cancel)
                return Outcome::Cancel;
            return Outcome::Continue;
        },
        -1);
    if (outcome == Outcome::Accept)
        fields = state.fields();
    return outcome == Outcome::Accept;
}

agent::Approval ModalHost::approve(const std::string& summary, int timeout_sec, int default_idx) {
    ApprovalModel model(timeout_sec, default_idx, ApprovalModel::steady_seconds);
    DialogView view;
    view.title = "Approve action?";
    view.lines = {summary};
    view.footer = "1-4: pick   Tab/left/right: move   Enter: confirm   Esc: cancel";
    view.buttons = approval_buttons(model.remaining_sec());
    view.selected_button = model.selection();

    const Outcome outcome = run(
        view,
        [&](int ch) {
            if (ch == '\t' || ch == keys::kRight || ch == keys::kLeft) {
                model.select(model.selection() + (ch == keys::kLeft ? -1 : 1));
            } else if (ch >= '1' && ch <= '4') {
                model.select(ch - '1');
                view.selected_button = model.selection();
                return Outcome::Accept;
            } else if (ch == '\n' || ch == '\r' || ch == keys::kEnter) {
                return Outcome::Accept;
            } else if (ch == 27) {
                return Outcome::Cancel;
            }
            view.selected_button = model.selection();
            view.buttons = approval_buttons(model.remaining_sec());
            return Outcome::Continue;
        },
        250,
        [&] {
            model.poll();
            if (model.timed_out())
                return true; // fail safe: deny, never auto-confirm
            view.buttons = approval_buttons(model.remaining_sec());
            view.selected_button = model.selection();
            return false;
        });
    if (outcome == Outcome::Cancel || model.timed_out())
        return model.resolve(-1);
    return model.resolve(model.selection());
}

ModalHost::Outcome ModalHost::run(DialogView& view, const Handler& handle, int poll_ms,
                                  const Tick& tick) {
    ModalScope modal;
    Terminal terminal;
    terminal.adopt();
    struct Restore {
        Terminal& terminal;
        ~Restore() { terminal.set_timeout(kTickTimeoutMs); }
    } restore{terminal};

    for (;;) {
        const PaintedDialog painted = paint_dialog(view, terminal.size());
        terminal.overlay(painted.surface, painted.x, painted.y, painted.cursor_x, painted.cursor_y);
        terminal.set_timeout(poll_ms);
        const int key = terminal.read_key();
        if (key == keys::kResize)
            continue;
        if (key == keys::kNone) {
            if (tick && tick()) {
                terminal.close_overlay();
                return Outcome::Cancel;
            }
            continue;
        }
        const Outcome outcome = handle(key);
        if (outcome == Outcome::Continue)
            continue;
        terminal.close_overlay();
        return outcome;
    }
}

} // namespace tui
