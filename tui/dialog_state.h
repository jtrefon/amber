#ifndef AMBER_TUI_DIALOG_STATE_H
#define AMBER_TUI_DIALOG_STATE_H

// Pure state machines for the modal sheets. One key in, one decision out; the
// host owns the terminal and the painter owns the pixels. The form reuses
// form_focus's documented key map so its behaviour cannot drift from the old
// libform dialog.

#include "tui/form_focus.h"
#include "tui/widgets.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tui {

enum class DialogKey : std::uint8_t { None, Redraw, Accept, Cancel };

class ConfirmState {
public:
    DialogKey key(int ch);
    bool yes() const noexcept { return yes_; }

private:
    bool yes_ = false;
};

// Read-only text: Up/Down/Page move the viewport, Enter/Esc/q close.
class InfoState {
public:
    DialogKey key(int ch, int visible, int total);
    int top() const noexcept { return top_; }

private:
    int top_ = 0;
};

class FormState {
public:
    explicit FormState(std::vector<FieldSpec> fields, std::string notice = {});

    DialogKey key(int ch);
    // Paste is not a key: insert the text at the focused field's cursor.
    void insert_text(const std::string& text);

    const std::vector<FieldSpec>& fields() const noexcept { return fields_; }
    const std::string& notice() const noexcept { return notice_; }

    bool on_field() const noexcept { return zone_ == form_focus::Zone::Fields; }
    bool ok_focused() const noexcept { return zone_ == form_focus::Zone::Ok; }
    bool cancel_focused() const noexcept { return zone_ == form_focus::Zone::Cancel; }
    std::size_t field_index() const noexcept { return focus_; }
    std::size_t cursor() const noexcept;
    const FieldSpec* focused_field() const noexcept;

    bool accepted() const noexcept { return accepted_; }
    bool cancelled() const noexcept { return cancelled_; }

private:
    void apply_intent(const form_focus::Decision& decision);
    void apply_focus(form_focus::Intent intent);
    void apply_edit(const form_focus::Decision& decision);
    void move_cursor(form_focus::Intent intent);
    void edit_text(const form_focus::Decision& decision);
    void erase_before();
    void erase_at();
    void insert_byte(unsigned char value);
    void clamp_cursor();

    std::vector<FieldSpec> fields_;
    std::vector<std::size_t> cursors_;
    std::string notice_;
    form_focus::Zone zone_ = form_focus::Zone::Fields;
    std::size_t focus_ = 0;
    bool accepted_ = false;
    bool cancelled_ = false;
};

} // namespace tui

#endif // AMBER_TUI_DIALOG_STATE_H
