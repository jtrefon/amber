#include "tui/dialog_state.h"

#include "tui/keys.h"

#include <algorithm>

namespace tui {

DialogKey ConfirmState::key(int ch) {
    if (ch == '\t' || ch == keys::kLeft || ch == keys::kRight || ch == keys::kUp ||
        ch == keys::kDown) {
        yes_ = !yes_;
        return DialogKey::Redraw;
    }
    if (ch == '\n' || ch == '\r' || ch == keys::kEnter)
        return DialogKey::Accept;
    if (ch == 27)
        return DialogKey::Cancel;
    return DialogKey::None;
}

DialogKey InfoState::key(int ch, int visible, int total) {
    const int page = std::max(1, visible - 1);
    const int max_top = std::max(0, total - visible);
    int next;
    switch (ch) {
    case keys::kDown:
        next = std::min(top_ + 1, max_top);
        break;
    case keys::kUp:
        next = std::max(top_ - 1, 0);
        break;
    case keys::kNPage:
        next = std::min(top_ + page, max_top);
        break;
    case keys::kPPage:
        next = std::max(top_ - page, 0);
        break;
    case keys::kHome:
        next = 0;
        break;
    case keys::kEnd:
        next = max_top;
        break;
    case '\n':
    case '\r':
    case keys::kEnter:
    case 27:
    case 'q':
    case 'Q':
        return DialogKey::Accept;
    default:
        return DialogKey::None;
    }
    if (next == top_)
        return DialogKey::None;
    top_ = next;
    return DialogKey::Redraw;
}

FormState::FormState(std::vector<FieldSpec> fields, std::string notice)
    : fields_(std::move(fields)), notice_(std::move(notice)) {
    cursors_.resize(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i)
        cursors_[i] = fields_[i].value.size();
}

std::size_t FormState::cursor() const noexcept {
    if (focus_ >= cursors_.size())
        return 0;
    return cursors_[focus_];
}

const FieldSpec* FormState::focused_field() const noexcept {
    if (!on_field() || focus_ >= fields_.size())
        return nullptr;
    return &fields_[focus_];
}

void FormState::clamp_cursor() {
    if (focus_ >= cursors_.size())
        return;
    cursors_[focus_] = std::min(cursors_[focus_], fields_[focus_].value.size());
}

void FormState::erase_before() {
    if (!on_field() || focus_ >= fields_.size())
        return;
    std::string& value = fields_[focus_].value;
    std::size_t& cursor = cursors_[focus_];
    cursor = std::min(cursor, value.size());
    if (cursor == 0)
        return;
    value.erase(cursor - 1, 1);
    --cursor;
}

void FormState::erase_at() {
    if (!on_field() || focus_ >= fields_.size())
        return;
    std::string& value = fields_[focus_].value;
    std::size_t& cursor = cursors_[focus_];
    cursor = std::min(cursor, value.size());
    if (cursor < value.size())
        value.erase(cursor, 1);
}

void FormState::insert_byte(unsigned char value) {
    if (!on_field() || focus_ >= fields_.size())
        return;
    std::string& text = fields_[focus_].value;
    std::size_t& cursor = cursors_[focus_];
    text.insert(cursor, 1, static_cast<char>(value));
    ++cursor;
}

void FormState::insert_text(const std::string& text) {
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 32 || byte == 127 || byte == 9 || byte == 10 || byte == 13)
            continue;
        insert_byte(byte);
    }
}

DialogKey FormState::key(int ch) {
    if (accepted_ || cancelled_)
        return DialogKey::None;
    const bool at_last_field = !fields_.empty() && focus_ + 1 >= fields_.size();
    const form_focus::Decision decision = form_focus::key(ch, zone_, at_last_field);
    zone_ = decision.zone;
    apply_intent(decision);
    clamp_cursor();
    if (decision.done) {
        accepted_ = decision.result;
        cancelled_ = !decision.result;
        return decision.result ? DialogKey::Accept : DialogKey::Cancel;
    }
    if (decision.intent != form_focus::Intent::None || decision.zone != form_focus::Zone::Fields)
        return DialogKey::Redraw;
    return DialogKey::None;
}

void FormState::apply_intent(const form_focus::Decision& decision) {
    using Intent = form_focus::Intent;
    switch (decision.intent) {
    case Intent::NextField:
    case Intent::PrevField:
    case Intent::ToButtons:
    case Intent::ToLastField:
        apply_focus(decision.intent);
        break;
    case Intent::PrevChar:
    case Intent::NextChar:
    case Intent::BegLine:
    case Intent::EndLine:
    case Intent::DelChar:
    case Intent::DelPrev:
    case Intent::InsertChar:
        apply_edit(decision);
        break;
    default:
        break;
    }
}

void FormState::apply_focus(form_focus::Intent intent) {
    switch (intent) {
    case form_focus::Intent::NextField:
        if (focus_ + 1 < fields_.size())
            ++focus_;
        break;
    case form_focus::Intent::PrevField:
        if (focus_ > 0)
            --focus_;
        break;
    case form_focus::Intent::ToButtons:
        zone_ = form_focus::Zone::Ok;
        break;
    case form_focus::Intent::ToLastField:
        zone_ = form_focus::Zone::Fields;
        if (!fields_.empty())
            focus_ = fields_.size() - 1;
        break;
    default:
        break;
    }
}

void FormState::apply_edit(const form_focus::Decision& decision) {
    switch (decision.intent) {
    case form_focus::Intent::PrevChar:
    case form_focus::Intent::NextChar:
    case form_focus::Intent::BegLine:
    case form_focus::Intent::EndLine:
        move_cursor(decision.intent);
        break;
    case form_focus::Intent::DelChar:
        erase_at();
        break;
    case form_focus::Intent::DelPrev:
        erase_before();
        break;
    case form_focus::Intent::InsertChar:
        if (decision.insert >= 32 && decision.insert <= 126)
            insert_byte(static_cast<unsigned char>(decision.insert));
        break;
    default:
        break;
    }
}

void FormState::move_cursor(form_focus::Intent intent) {
    if (!on_field() || focus_ >= cursors_.size())
        return;
    std::size_t& cursor = cursors_[focus_];
    const std::size_t size = fields_[focus_].value.size();
    switch (intent) {
    case form_focus::Intent::PrevChar:
        if (cursor > 0)
            --cursor;
        break;
    case form_focus::Intent::NextChar:
        if (cursor < size)
            ++cursor;
        break;
    case form_focus::Intent::BegLine:
        cursor = 0;
        break;
    case form_focus::Intent::EndLine:
        cursor = size;
        break;
    default:
        break;
    }
}

void FormState::edit_text(const form_focus::Decision& decision) {
    switch (decision.intent) {
    case form_focus::Intent::DelChar:
        erase_at();
        break;
    case form_focus::Intent::DelPrev:
        erase_before();
        break;
    case form_focus::Intent::InsertChar:
        if (decision.insert >= 32 && decision.insert <= 126)
            insert_byte(static_cast<unsigned char>(decision.insert));
        break;
    default:
        break;
    }
}

} // namespace tui
