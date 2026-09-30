
#include "command_line.h"

#include <algorithm>
#include <cctype>

namespace tui {

namespace {

// Shared completion match: `partial` matches `name` when it is a prefix,
// or (for dotted partials) when the suffix after the last dot matches.
// An empty partial matches everything (the drawer rows are the filter).
bool completion_matches(const std::string& name, const std::string& p) {
    if (p.empty())
        return true;
    if (name.size() >= p.size() && name.substr(0, p.size()) == p)
        return true;
    size_t dot = p.rfind('.');
    if (dot == std::string::npos || dot + 1 >= p.size())
        return false;
    const std::string suffix = p.substr(dot + 1);
    return name.size() >= suffix.size() && name.substr(0, suffix.size()) == suffix;
}

} // namespace

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
    if (cycle_matches_.empty())
        return;
    if (dir > 0)
        cycle_index_ = (cycle_index_ + 1) % cycle_matches_.size();
    else
        cycle_index_ = (cycle_index_ + cycle_matches_.size() - 1) % cycle_matches_.size();

    // Replace the current token with the cycled match.
    // Find the start of the current word.
    size_t start = cursor_;
    while (start > 0 && input_[start - 1] != ' ')
        --start;
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

// The token after the last dot: for "set.policy.mode" the user is completing
// "mode".
std::string completion_token(const std::string& partial) {
    const size_t dot = partial.rfind('.');
    return (dot == std::string::npos || dot + 1 >= partial.size()) ? partial
                                                                   : partial.substr(dot + 1);
}

// The shadow text for a match: the rest of the name, or a space when it is
// already complete.
std::string shadow_for(const std::string& match, const std::string& token) {
    return match.size() > token.size() ? match.substr(token.size()) : " ";
}

// The shadow from Tab cycling, or empty when the current cycle entry no longer
// extends the partial.
std::string CommandLine::cycle_shadow(const std::string& partial) const {
    if (cycle_matches_.empty() || cycle_index_ >= cycle_matches_.size())
        return "";
    const std::string& match = cycle_matches_[cycle_index_];
    const std::string token = completion_token(partial);
    if (token.empty() || match.size() < token.size() || match.substr(0, token.size()) != token)
        return "";
    return shadow_for(match, token);
}

// The shadow from the current completion context, or empty when nothing matches.
std::string CommandLine::completion_shadow(const std::string& partial) const {
    for (const auto& name : completions_)
        if (completion_matches(name, partial))
            return shadow_for(name, completion_token(partial));
    return "";
}

void CommandLine::recompute() {
    shadow_.clear();

    // Update drawer state — it should close when / is deleted.
    drawer_open_ = (!input_.empty() && input_[0] == '/');

    // Only show shadow when cursor is at end of input.
    if (cursor_ != input_.size())
        return;
    if (input_.empty() || input_[0] != '/')
        return;

    // Find the partial token (text after last space or start).
    size_t tok_start = input_.rfind(' ');
    tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
    const std::string partial = input_.substr(tok_start);
    if (partial.empty())
        return;

    // First, try cycle matches (set by Tab cycling), then fall back to the
    // current completion context.
    shadow_ = cycle_shadow(partial);
    if (shadow_.empty())
        shadow_ = completion_shadow(partial);
}

// ── Event handlers ──────────────────────────────────────────────────

// The drawer's visible items: the completions the host set. The host
// (drawer_entry_names) has ALREADY applied the namespace-descend and the
// prefix filter, so this list is exactly the rows the drawer renders — arrow
// navigation and Enter dispatch index the same rows the user sees. Re-filtering
// here against the raw input token is wrong: for an exact-command descend
// ("/window" -> children new/close/list/rename) the trailing token is a
// consumed namespace, not a partial, so re-filtering would empty the list and
// send arrows into history navigation.
std::vector<std::string> CommandLine::drawer_items() const {
    return completions_;
}

// '?' on a slash command: a full help page when it follows a space (for the path
// before it), an inline remaining-options popup otherwise. The '?' is inserted
// first so the popup path can restore the input verbatim.
CommandLine::Result CommandLine::intercept_help_question(char c) {
    Result r;
    const bool has_space = (cursor_ >= 2 && input_[cursor_ - 1] == ' ');
    const std::string before_q = input_;
    const size_t before_cursor = cursor_;
    input_.insert(cursor_, 1, c);
    ++cursor_;

    if (!has_space) {
        // No space → inline remaining-options popup: the input before ? is
        // valid, and the caller populates the items from the token before ?.
        r.action = Result::ShowPopup;
        input_ = before_q;
        cursor_ = before_cursor;
        return r;
    }
    // Space before ? → full help page for the path before the space. Strip the ?
    // and everything after the space before it.
    const size_t sp = before_q.rfind(' ');
    const std::string path = before_q.substr(0, sp);
    r.action = Result::ShowHelpPage;
    r.help_node = path;
    // Restore input to before the ? was typed (without trailing space).
    input_ = path + " ";
    cursor_ = input_.size();
    drawer_open_ = false;
    return r;
}

CommandLine::Result CommandLine::on_char(char c) {
    Result r;
    save_undo();

    if (c == '?' && !input_.empty() && input_[0] == '/')
        return intercept_help_question(c);

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
    }
    return r;
}

// Begin (or restart) Tab cycling over the current completions.
void CommandLine::start_cycle() {
    if (!completions_.empty() && cycle_matches_.empty()) {
        cycle_matches_ = completions_;
        cycle_index_ = 0;
        consecutive_tabs_ = 1;
        last_tab_input_ = input_;
        return;
    }
    if (!cycle_matches_.empty()) {
        cycle_index_ = 0;
        consecutive_tabs_ = 1;
    }
}

// True when the partial token after the last space could still be completed:
// empty, ending at a dot, or a prefix of something offered.
bool CommandLine::can_complete_partial() const {
    size_t tok_start = input_.rfind(' ');
    tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
    const std::string partial = input_.substr(tok_start);
    if (partial.empty() || partial.back() == '.')
        return true;
    return std::any_of(completions_.begin(), completions_.end(),
                       [&](const std::string& n) { return n.rfind(partial, 0) == 0; });
}

// No shadow, but completions exist (e.g. empty suffix after dot). Only append
// when the partial token could match: an unrelated partial (e.g. "/set model"
// with model-id completions) must not concatenate.
bool CommandLine::accept_first_completion() {
    if (completions_.empty() || !can_complete_partial())
        return false;
    input_ += completions_[0];
    cursor_ = input_.size();
    cycle_matches_ = completions_;
    cycle_index_ = 0;
    consecutive_tabs_ = 1;
    last_tab_input_ = input_;
    recompute();
    return true;
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

    // Accept shadow if present, then start cycling.
    if (!shadow_.empty()) {
        input_ += shadow_;
        cursor_ = input_.size();
        shadow_.clear();
        start_cycle();
        recompute();
        return r;
    }

    accept_first_completion();
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
    if (input_.empty())
        return r;

    // If drawer is open with selection, dispatch the selected command.
    // Match against the filtered drawer items (not cycle_matches_, which may
    // be stale or unfiltered from Tab cycling).
    if (drawer_open_ && drawer_sel_ >= 0) {
        std::vector<std::string> filtered = drawer_items();
        if (drawer_sel_ < static_cast<int>(filtered.size())) {
            r.action = Result::Dispatch;
            // Preserve the typed prefix. When the host supplied an explicit
            // dispatch prefix ("/window " for a namespace descend) use it;
            // otherwise keep the legacy input-derived prefix (everything up
            // to the trailing partial token).
            std::string prefix;
            if (!dispatch_prefix_.empty()) {
                prefix = dispatch_prefix_;
            } else {
                size_t tok_start = input_.rfind(' ');
                tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
                prefix = input_.substr(0, tok_start);
            }
            r.dispatch_text = prefix + filtered[drawer_sel_];
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
        if (history_.size() > 100)
            history_.erase(history_.begin());
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
        while (start > 0 && input_[start - 1] == ' ')
            --start;
        // Skip non-space backward.
        while (start > 0 && input_[start - 1] != ' ')
            --start;
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
    // Drawer open: move the highlight up through the filtered rows. The
    // arrows navigate the drawer independently of Tab (cycle_matches_ is
    // only populated once Tab is pressed); Enter dispatches the highlight.
    if (drawer_open_) {
        auto filtered = drawer_items();
        if (!filtered.empty()) {
            if (drawer_sel_ > 0) {
                --drawer_sel_;
            } else {
                drawer_sel_ = static_cast<int>(filtered.size()) - 1; // wrap
            }
            recompute();
            return r;
        }
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
    // Drawer open: move the highlight down through the filtered rows.
    if (drawer_open_) {
        auto filtered = drawer_items();
        if (!filtered.empty()) {
            if (drawer_sel_ < static_cast<int>(filtered.size()) - 1) {
                ++drawer_sel_;
            } else {
                drawer_sel_ = 0; // wrap
            }
            recompute();
            return r;
        }
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
    if (cursor_ > 0)
        --cursor_;
    return Result{};
}

CommandLine::Result CommandLine::on_right() {
    if (cursor_ < input_.size())
        ++cursor_;
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
