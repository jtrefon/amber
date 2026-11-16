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

std::vector<DialogOption> approval_options() {
    return {
        {"1", "Allow once"}, {"2", "Allow session"}, {"3", "Always allow"}, {"4", "Always deny"}};
}

std::string approval_status(int timeout_sec, int remaining) {
    if (timeout_sec <= 0)
        return {};
    return "auto-deny in " + std::to_string(std::max(0, remaining)) + "s";
}

DialogView approval_view(const ApprovalModel& model, int timeout_sec) {
    DialogView view;
    view.title = "Approve action?";
    view.footer_keys = {{"1-4", "Pick"}, {"Tab", "Move"}, {"Enter", "Confirm"}, {"Esc", "Cancel"}};
    view.options = approval_options();
    view.selected_option = model.selection();
    view.status = approval_status(timeout_sec, model.remaining_sec());
    view.preferred_width = 62;
    return view;
}

} // namespace

bool ModalHost::confirm(const std::string& title, const std::string& message) {
    ConfirmState state;
    DialogView view;
    view.title = title;
    view.lines = {message};
    view.buttons = {"[ Yes ]", "[ No ]"};
    view.selected_button = 1; // the safe choice starts focused
    view.footer_keys = {{"Tab", "Switch"}, {"Enter", "Confirm"}, {"Esc", "Cancel"}};

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
    view.footer_keys = {
        {"Up/Down", "Move"}, {"Enter", "Select"}, {"/", "Filter"}, {"Esc", "Cancel"}};
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
    view.footer_keys = {{"Up/Down", "Scroll"}, {"Esc", "Close"}};
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
    view.footer_keys = {{"Tab", "Next"}, {"Enter", "Confirm"}, {"Esc", "Cancel"}};
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
    DialogView view = approval_view(model, timeout_sec);
    view.lines = {summary};

    const Outcome outcome = run(
        view,
        [&](int ch) {
            if (ch == '\t' || ch == keys::kRight || ch == keys::kLeft) {
                model.select(model.selection() + (ch == keys::kLeft ? -1 : 1));
            } else if (ch >= '1' && ch <= '4') {
                model.select(ch - '1');
                view.selected_option = model.selection();
                return Outcome::Accept;
            } else if (ch == '\n' || ch == '\r' || ch == keys::kEnter) {
                return Outcome::Accept;
            } else if (ch == 27) {
                return Outcome::Cancel;
            }
            view.selected_option = model.selection();
            view.status = approval_status(timeout_sec, model.remaining_sec());
            return Outcome::Continue;
        },
        250,
        [&] {
            model.poll();
            if (model.timed_out())
                return true; // fail safe: deny, never auto-confirm
            view.selected_option = model.selection();
            view.status = approval_status(timeout_sec, model.remaining_sec());
            return false;
        });
    if (outcome == Outcome::Cancel || model.timed_out())
        return model.resolve(-1);
    return model.resolve(model.selection());
}

ModalHost::Outcome ModalHost::run(DialogView& view, const Handler& handle, int poll_ms,
                                  const Tick& tick) {
    Outcome outcome = Outcome::Continue;
    const DialogKey result = loop([&view](Size size) { return paint_dialog(view, size); },
                                  [&handle, &outcome](int key) {
                                      outcome = handle(key);
                                      if (outcome == Outcome::Accept)
                                          return DialogKey::Accept;
                                      if (outcome == Outcome::Cancel)
                                          return DialogKey::Cancel;
                                      return DialogKey::None;
                                  },
                                  poll_ms, tick);
    return result == DialogKey::Accept ? Outcome::Accept : Outcome::Cancel;
}

void ModalHost::custom(const std::function<PaintedDialog(Size)>& painter,
                       const std::function<DialogKey(int)>& handler, int poll_ms,
                       const std::function<bool()>& tick) {
    loop(painter, handler, poll_ms, tick);
}

DialogKey ModalHost::loop(const std::function<PaintedDialog(Size)>& painter,
                          const std::function<DialogKey(int)>& handler, int poll_ms,
                          const Tick& tick) {
    ModalScope modal;
    Terminal terminal;
    terminal.adopt();
    struct Restore {
        Terminal& terminal;
        ~Restore() { terminal.set_timeout(kTickTimeoutMs); }
    } restore{terminal};

    for (;;) {
        const PaintedDialog painted = painter(terminal.size());
        terminal.overlay(painted.surface, painted.x, painted.y, painted.cursor_x, painted.cursor_y);
        terminal.set_timeout(poll_ms);
        const int key = terminal.read_key();
        if (key == keys::kResize)
            continue;
        if (key == keys::kNone) {
            if (tick && tick()) {
                terminal.close_overlay();
                return DialogKey::Cancel;
            }
            continue;
        }
        const DialogKey action = handler(key);
        if (action == DialogKey::Accept || action == DialogKey::Cancel) {
            terminal.close_overlay();
            return action;
        }
    }
}

} // namespace tui
