
#include "command_line.h"

#include <algorithm>
#include <cctype>

namespace tui {

CommandLine::CommandLine() = default;

// ── Internal helpers ────────────────────────────────────────────────

void CommandLine::save_undo() {
    undo_buffer_ = input_;
    undo_cursor_ = cursor_;
}

void CommandLine::reset_cycle() {
    cycle_matches_.clear();
    cycle_index_ = 0;
    drawer_sel_ = 0;
    consecutive_tabs_ = 0;
    last_tab_input_.clear();
}


void CommandLine::advance_cycle(int dir) {
    if (cycle_matches_.empty()) return;
    if (dir > 0) cycle_index_ = (cycle_index_ + 1) % cycle_matches_.size();
    else cycle_index_ = (cycle_index_ + cycle_matches_.size() - 1) % cycle_matches_.size();

    // Replace the current token with the cycled match.
    // Find the start of the current word.
    size_t start = cursor_;
    while (start > 0 && input_[start - 1] != ' ') --start;
    std::string token = input_.substr(start, cursor_ - start);
    std::string replacement = cycle_matches_[cycle_index_];
    // For dotted tokens where completions are leaf-level, preserve the prefix.
    size_t dot = token.rfind('.');
    if (dot != std::string::npos) {
        std::string prefix = token.substr(0, dot + 1);
        replacement = prefix + replacement;
    }
    input_.replace(start, cursor_ - start, replacement);
    cursor_ = start + replacement.size();
    drawer_sel_ = static_cast<int>(cycle_index_);
}

void CommandLine::recompute() {
    shadow_.clear();

    // Update drawer state — it should close when / is deleted.
    drawer_open_ = (!input_.empty() && input_[0] == '/');

    // Only show shadow when cursor is at end of input.
    if (cursor_ != input_.size()) return;
    if (input_.empty() || input_[0] != '/') return;

    // Find the partial token (text after last space or start).
    size_t tok_start = input_.rfind(' ');
    tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
    std::string partial = input_.substr(tok_start);
    if (partial.empty()) return;

    // Helper: match `partial` against a candidate completion `name`.
    // When partial is dotted (e.g. "detection.l") but completions are
    // leaf-level ("loop"), try matching only the suffix after the last dot.
    auto matches = [&](const std::string& name, const std::string& p) -> bool {
        if (p.empty()) return false;
        if (name.size() >= p.size() && name.substr(0, p.size()) == p)
            return true;
        size_t dot = p.rfind('.');
        if (dot == std::string::npos || dot + 1 >= p.size()) return false;
        std::string suffix = p.substr(dot + 1);
        return name.size() >= suffix.size() &&
               name.substr(0, suffix.size()) == suffix;
    };

    // First, try cycle matches (set by Tab cycling).
    if (!cycle_matches_.empty() && cycle_index_ < cycle_matches_.size()) {
        const std::string& match = cycle_matches_[cycle_index_];
        size_t dot = partial.rfind('.');
        std::string p = (dot == std::string::npos || dot + 1 >= partial.size())
                        ? partial : partial.substr(dot + 1);
        if (!p.empty() && match.size() >= p.size() && match.substr(0, p.size()) == p) {
            if (match.size() > p.size())
                shadow_ = match.substr(p.size());
            else
                shadow_ = " ";
            return;
        }
    }

    // Fallback: compute shadow from the current completion context.
    for (const auto& name : completions_) {
        if (matches(name, partial)) {
            size_t dot = partial.rfind('.');
            std::string p = (dot == std::string::npos || dot + 1 >= partial.size())
                            ? partial : partial.substr(dot + 1);
            if (name.size() > p.size()) {
                shadow_ = name.substr(p.size());
            } else {
                shadow_ = " ";
            }
            return;
        }
    }
}

// ── Event handlers ──────────────────────────────────────────────────

CommandLine::Result CommandLine::on_char(char c) {
    Result r;
    save_undo();

    if (c == '?' && !input_.empty() && input_[0] == '/') {
        // ? interception: check if preceded by space.
        bool has_space = (cursor_ >= 2 && input_[cursor_ - 1] == ' ');
        std::string before_q = input_;
        size_t before_cursor = cursor_;

        // Insert the ? (we'll strip it if we intercept).
        input_.insert(cursor_, 1, c);
        ++cursor_;

        if (has_space) {
            // Space before ? → full help page for path before the space.
            // Strip the ? and everything after the space before it.
            size_t sp = before_q.rfind(' ');
            std::string path = before_q.substr(0, sp);
            r.action = Result::ShowHelpPage;
            r.help_node = path;
            // Restore input to before the ? was typed (without trailing space).
            input_ = path + " ";
            cursor_ = input_.size();
            drawer_open_ = false;
            return r;
        }
        {
            // No space → inline remaining-options popup.
            // The input before ? is valid; show completion options.
            r.action = Result::ShowPopup;
            // For now, use palette-style filtering.
            // Popup items are set by the caller based on the token before ?.
            // Restore input to before ?.
            input_ = before_q;
            cursor_ = before_cursor;
            // Signal the caller to show popup for the token before ?.
            return r;
        }
    }

    if (c == '@' && input_.size() < 65536) {
        // @ reference: insert @ and show file popup.
        input_.insert(cursor_, 1, c);
        ++cursor_;
        r.action = Result::ShowPopup;
        // The caller will populate popup_items with workspace files.
        return r;
    }

    // Regular printable character.
    if (c >= 32 && c <= 126 && input_.size() < 65536) {
        input_.insert(cursor_, 1, c);
        ++cursor_;
        reset_cycle();
        drawer_open_ = (!input_.empty() && input_[0] == '/');
        recompute();
        return r;
    }

    return r;
}

CommandLine::Result CommandLine::on_tab() {
    Result r;

    // If we have an active cycle, advance forward.
    if (!cycle_matches_.empty()) {
        advance_cycle(1);
        drawer_sel_ = static_cast<int>(cycle_index_);
        drawer_open_ = true;
        recompute();
        return r;
    }

    // Accept shadow if present.
    if (!shadow_.empty()) {
        input_ += shadow_;
        cursor_ = input_.size();
        shadow_.clear();
        // Start cycling with current completions.
        if (!completions_.empty() && cycle_matches_.empty()) {
            cycle_matches_ = completions_;
            cycle_index_ = 0;
            consecutive_tabs_ = 1;
            last_tab_input_ = input_;
        } else if (!cycle_matches_.empty()) {
            cycle_index_ = 0;
            consecutive_tabs_ = 1;
        }
        recompute();
        return r;
    }

    // No shadow, but completions exist (e.g. empty suffix after dot).
    if (!completions_.empty()) {
        input_ += completions_[0];
        cursor_ = input_.size();
        cycle_matches_ = completions_;
        cycle_index_ = 0;
        consecutive_tabs_ = 1;
        last_tab_input_ = input_;
        recompute();
        return r;
    }

    // Nothing to complete.
    return r;
}

CommandLine::Result CommandLine::on_shift_tab() {
    Result r;
    if (!cycle_matches_.empty()) {
        advance_cycle(-1);
        drawer_sel_ = static_cast<int>(cycle_index_);
        recompute();
    }
    return r;
}

CommandLine::Result CommandLine::on_enter() {
    Result r;
    if (input_.empty()) return r;

    // If drawer is open with selection, dispatch the selected command.
    // Match against completions_ (filtered) rather than cycle_matches_
    // (which may be stale or unfiltered from Tab cycling).
    if (drawer_open_ && drawer_sel_ >= 0) {
        size_t tok_start = input_.rfind(' ');
        tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
        std::string partial = input_.substr(tok_start);
        auto matches = [&](const std::string& name, const std::string& p) -> bool {
            if (p.empty()) return false;
            if (name.size() >= p.size() && name.substr(0, p.size()) == p)
                return true;
            size_t dot = p.rfind('.');
            if (dot == std::string::npos || dot + 1 >= p.size()) return false;
            std::string suffix = p.substr(dot + 1);
            return name.size() >= suffix.size() &&
                   name.substr(0, suffix.size()) == suffix;
        };
        std::vector<std::string> filtered;
        for (const auto& name : completions_) {
            if (matches(name, partial))
                filtered.push_back(name);
        }
        if (drawer_sel_ < static_cast<int>(filtered.size())) {
            r.action = Result::Dispatch;
            r.dispatch_text = "/" + filtered[drawer_sel_];
            r.drawer_open = false;
            input_.clear();
            cursor_ = 0;
            reset_cycle();
            return r;
        }
    }

    // Dispatch whatever is in the input.
    r.action = Result::Dispatch;
    r.dispatch_text = input_;
    // Save to history.
    if (history_.empty() || history_.back() != input_) {
        history_.push_back(input_);
        if (history_.size() > 100) history_.erase(history_.begin());
    }
    history_pos_ = history_.size();
    input_.clear();
    cursor_ = 0;
    drawer_open_ = false;
    reset_cycle();
    return r;
}

CommandLine::Result CommandLine::on_backspace() {
    Result r;
    save_undo();
    if (cursor_ > 0 && !input_.empty()) {
        --cursor_;
        input_.erase(cursor_, 1);
    }
    reset_cycle();
    recompute();
    return r;
}

CommandLine::Result CommandLine::on_ctrl_d() {
    Result r;
    if (!input_.empty() && input_[0] == '/') {
        r.action = Result::ShowPopup;
        // Caller populates popup_items with completions.
    }
    return r;
}

CommandLine::Result CommandLine::on_ctrl_r() {
    // Stub — full implementation deferred.
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_a() {
    cursor_ = 0;
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_e() {
    cursor_ = input_.size();
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_w() {
    save_undo();
    if (cursor_ > 0) {
        size_t start = cursor_;
        // Skip spaces backward.
        while (start > 0 && input_[start - 1] == ' ') --start;
        // Skip non-space backward.
        while (start > 0 && input_[start - 1] != ' ') --start;
        kill_buffer_ = input_.substr(start, cursor_ - start);
        input_.erase(start, cursor_ - start);
        cursor_ = start;
    }
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_u() {
    save_undo();
    input_.erase(0, cursor_);
    cursor_ = 0;
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_k() {
    save_undo();
    input_.erase(cursor_);
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_y() {
    save_undo();
    if (!kill_buffer_.empty()) {
        input_.insert(cursor_, kill_buffer_);
        cursor_ += kill_buffer_.size();
    }
    return Result{};
}

CommandLine::Result CommandLine::on_ctrl_t() {
    save_undo();
    if (cursor_ > 0 && cursor_ < input_.size()) {
        std::swap(input_[cursor_ - 1], input_[cursor_]);
        ++cursor_;
    }
    return Result{};
}

CommandLine::Result CommandLine::on_undo() {
    std::swap(input_, undo_buffer_);
    std::swap(cursor_, undo_cursor_);
    return Result{};
}

CommandLine::Result CommandLine::on_up() {
    Result r;
    // If drawer is open with matches, cycle up through them.
    if (drawer_open_ && !cycle_matches_.empty()) {
        return on_shift_tab();
    }
    // Otherwise, history.
    if (!history_.empty() && history_pos_ > 0) {
        --history_pos_;
        input_ = history_[history_pos_];
        cursor_ = input_.size();
    }
    return r;
}

CommandLine::Result CommandLine::on_down() {
    Result r;
    if (drawer_open_ && !cycle_matches_.empty()) {
        return on_tab();
    }
    if (!history_.empty() && history_pos_ < history_.size() - 1) {
        ++history_pos_;
        input_ = history_[history_pos_];
        cursor_ = input_.size();
    } else if (!history_.empty()) {
        history_pos_ = history_.size();
        input_.clear();
        cursor_ = 0;
    }
    return r;
}

CommandLine::Result CommandLine::on_left() {
    if (cursor_ > 0) --cursor_;
    return Result{};
}

CommandLine::Result CommandLine::on_right() {
    if (cursor_ < input_.size()) ++cursor_;
    return Result{};
}

CommandLine::Result CommandLine::on_home() {
    cursor_ = 0;
    return Result{};
}

CommandLine::Result CommandLine::on_end() {
    cursor_ = input_.size();
    return Result{};
}

} // namespace tui
