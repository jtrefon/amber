// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#pragma once

#include <string>
#include <vector>

namespace tui {

// Pure-logic input-line state machine. No ncurses, no IO.
// Fully testable by feeding events and checking the result.
//
// Usage:
//   CommandLine cl;
//   cl.on_char('s'); cl.on_char('e'); cl.on_char('t');
//   assert(cl.text() == "/set");
//   Result r = cl.on_char('?');
//   assert(r.action == Result::ShowHelpPage);

class CommandLine {
public:
    CommandLine();

    // Current state.
    const std::string& text() const { return input_; }
    size_t cursor() const { return cursor_; }
    bool drawer_open() const { return drawer_open_; }
    int  drawer_sel() const { return drawer_sel_; }
    const std::string& shadow() const { return shadow_; }
    const std::string& kill_buffer() const { return kill_buffer_; }

    // Result of an event.
    struct Result {
        enum Action {
            None,              // internal state change only
            Dispatch,          // enter pressed → dispatch dispatch_text
            ShowPopup,         // show popup with popup_items
            ShowHelpPage       // show full help page for help_node
        };
        Action action = None;
        std::string dispatch_text;
        std::vector<std::string> popup_items;
        std::string help_node;       // tree path to show help for

        bool drawer_open = false;
        int  drawer_sel = 0;
    };

    // ── Event handlers ──────────────────────────────────────────────

    Result on_char(char c);      // printable character
    Result on_tab();             // Tab
    Result on_shift_tab();       // Shift-Tab (backward cycle)
    Result on_enter();
    Result on_backspace();
    Result on_ctrl_d();
    Result on_ctrl_r();          // reverse history search (stub)
    Result on_ctrl_a();          // beginning of line
    Result on_ctrl_e();          // end of line
    Result on_ctrl_w();          // delete word backward
    Result on_ctrl_u();          // delete to start of line
    Result on_ctrl_k();          // delete to end of line
    Result on_ctrl_y();          // yank (paste kill buffer)
    Result on_ctrl_t();          // transpose characters
    Result on_undo();            // Ctrl-_ (undo)
    Result on_up();              // history up or cycle up
    Result on_down();            // history down or cycle down
    Result on_left();
    Result on_right();
    Result on_home();
    Result on_end();

    // ── History ─────────────────────────────────────────────────────

    void set_history(const std::vector<std::string>& h) { history_ = h; history_pos_ = history_.size(); }
    const std::vector<std::string>& history() const { return history_; }
    size_t history_pos() const { return history_pos_; }

    // ── Completion context (set by the host for shadow computation) ──

    // Provide the list of valid command/subcommand names at the current depth.
    // CommandLine uses these to compute the shadow (faded completion hint).
    void set_completions(const std::vector<std::string>& names) { completions_ = names; recompute(); }
    const std::vector<std::string>& completions() const { return completions_; }

    // ── Direct state control (for test setup) ───────────────────────

    void set_text(const std::string& t) { input_ = t; cursor_ = t.size(); recompute(); }
    void set_text_and_cursor(const std::string& t, size_t c) { input_ = t; cursor_ = c; }

private:
    std::string input_;
    size_t cursor_ = 0;
    bool drawer_open_ = false;
    int  drawer_sel_ = 0;
    std::string shadow_;           // faded completion hint after cursor
    std::string kill_buffer_;      // for Ctrl-Y (yank)
    std::string undo_buffer_;      // for undo
    size_t undo_cursor_ = 0;

    // Completion / cycle state
    std::vector<std::string> cycle_matches_;
    size_t cycle_index_ = 0;
    std::string last_tab_input_;
    int consecutive_tabs_ = 0;

    // Completion context (valid names at current depth)
    std::vector<std::string> completions_;

    // History
    std::vector<std::string> history_;
    size_t history_pos_ = 0;

    // Internal helpers
    void recompute();              // update shadow, drawer after mutation
    void save_undo();              // save state for undo
    void start_cycle(const std::vector<std::string>& matches);
    void advance_cycle(int dir);
    void reset_cycle();

    // @-reference helper
};

} // namespace tui
