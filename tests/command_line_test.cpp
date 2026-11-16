#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "tui/command_line.h"
#include "tui/draft_store.h"

// Minimal test framework for CommandLine.
#define TEST(name) void name()
#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n";       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))

int run_test(const char* name, int (*fn)()) {
    std::cout << "[TEST] " << name << "...\n";
    int rc = fn();
    if (rc)
        std::cout << "  FAIL\n";
    else
        std::cout << "  PASS\n";
    return rc;
}

// ── Test: basic text insertion ──
static int test_basic_insertion() {
    tui::CommandLine cl;
    cl.on_char('h');
    cl.on_char('e');
    cl.on_char('l');
    ASSERT_EQ(cl.text(), "hel");
    ASSERT_EQ(cl.cursor(), 3);
    return 0;
}

// ── Test: slash triggers drawer ──
static int test_slash_triggers_drawer() {
    tui::CommandLine cl;
    cl.on_char('/');
    ASSERT(cl.drawer_open());
    ASSERT_EQ(cl.text(), "/");
    return 0;
}

// ── Test: backspace ──
static int test_backspace() {
    tui::CommandLine cl;
    cl.set_text("hello");
    cl.on_backspace();
    cl.on_backspace();
    ASSERT_EQ(cl.text(), "hel");
    ASSERT_EQ(cl.cursor(), 3);
    return 0;
}

// ── Tests: provider tabs own Left/Right only while the drawer is open ──
//
// Left/Right normally move the text cursor. With provider tabs active and the
// drawer open they move between tabs instead, and Up/Down move the row within
// the tab. Closing the drawer must hand the arrows back, or a Left press in the
// scrollback silently does nothing.

static tui::CommandLine with_model_tabs() {
    tui::CommandLine cl;
    // Rows are composite provider::model keys, as the /set model feed emits them.
    cl.set_completions({"beta::beta-large", "beta::beta-small", "alpha::shared"}, "/set model ");
    // set_completions does not set the input, and the drawer only opens for a
    // slash input, so type the command the way a user would.
    cl.set_text("/set model ");
    cl.set_provider_tabs({"", "beta", "alpha"});
    return cl;
}

static int test_left_moves_tabs_when_the_drawer_is_open() {
    auto cl = with_model_tabs();
    ASSERT(cl.drawer_open());
    cl.on_right(); // All -> beta
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(1));
    cl.on_left(); // beta -> All
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(0));
    return 0;
}

static int test_left_at_the_leftmost_tab_does_nothing() {
    // Clamped, not wrapped: a wrapped Left reads as an unresponsive drawer. It
    // is still the DRAWER's key, though -- falling through to the caret made
    // Left "move the cursor in the prompt" on the All tab, where every user
    // starts. The slide was invisible on the right only because the caret
    // begins at the end of the line.
    auto cl = with_model_tabs();
    const std::size_t caret = cl.cursor();
    cl.on_left();
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(0));
    ASSERT_EQ(cl.cursor(), caret);
    return 0;
}

static int test_right_moves_towards_a_provider() {
    auto cl = with_model_tabs();
    cl.on_right();
    cl.on_right();
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(2)); // alpha
    cl.on_right();                                                   // clamped
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(2));
    // The clamped press is consumed too: with the caret at home it must stay
    // there rather than slide right.
    cl.on_home();
    cl.on_right();
    ASSERT_EQ(cl.cursor(), static_cast<std::size_t>(0));
    return 0;
}

static int test_arrows_return_to_cursor_movement_when_the_drawer_is_closed() {
    auto cl = with_model_tabs();
    ASSERT_EQ(cl.text(), "/set model ");
    cl.on_escape(); // close the drawer
    ASSERT(!cl.drawer_open());
    cl.on_left();
    // The cursor moved, no tab changed: the drawer gave the keys back.
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(0));
    ASSERT_EQ(cl.cursor(), cl.text().size() - 1);
    return 0;
}

static int test_tab_rows_are_filtered_to_the_selected_provider() {
    auto cl = with_model_tabs();
    ASSERT_EQ(cl.tab_rows().size(), static_cast<std::size_t>(3));
    cl.on_right(); // All -> beta
    const auto beta_rows = cl.tab_rows();
    ASSERT_EQ(beta_rows.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(beta_rows[0], std::string("beta::beta-large"));
    cl.on_right(); // beta -> alpha
    const auto alpha_rows = cl.tab_rows();
    ASSERT_EQ(alpha_rows.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(alpha_rows[0], std::string("alpha::shared"));
    return 0;
}

// Enter runs a row from the SELECTED tab: the row the drawer paints is the row
// that dispatches, so a tab switch changes what Enter sets. On All the first row
// is beta's; on the alpha tab it is alpha's.
static int test_enter_dispatches_from_the_selected_tab() {
    auto cl = with_model_tabs();
    cl.on_right(); // All -> beta
    cl.on_right(); // beta -> alpha
    const auto r = cl.on_enter();
    ASSERT_EQ(r.dispatch_text, std::string("/set model alpha::shared"));
    return 0;
}

// The host re-seeds the tabs on every keystroke (the counts follow the filter,
// and a provider with no hits leaves the list). The user's chosen tab survives
// by NAME, so neither the choice is lost nor the index silently re-points at
// whichever provider now holds that slot.
static int test_tab_selection_survives_reseeding_by_name() {
    auto cl = with_model_tabs();
    cl.on_right(); // All -> beta
    ASSERT_EQ(cl.provider_tab_provider(), std::string("beta"));

    cl.set_provider_tabs({"", "alpha", "beta"});                     // order changed under the user
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(2)); // still beta
    ASSERT_EQ(cl.provider_tab_provider(), std::string("beta"));

    cl.set_provider_tabs({"", "alpha"});                             // beta lost its last hit
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(0)); // back to All
    ASSERT_EQ(cl.provider_tab_provider(), std::string(""));
    return 0;
}

static int test_up_down_move_the_row_within_a_tab() {
    auto cl = with_model_tabs();
    cl.on_down();
    ASSERT_EQ(cl.drawer_sel(), 1);
    cl.on_up();
    ASSERT_EQ(cl.drawer_sel(), 0);
    return 0;
}

static int test_reopening_the_drawer_restores_the_tab() {
    auto cl = with_model_tabs();
    cl.on_right();
    cl.on_escape();
    cl.on_char('x');
    ASSERT(cl.drawer_open()); // still a slash input
    ASSERT_EQ(cl.provider_tab_index(), static_cast<std::size_t>(1));
    return 0;
}

static int test_no_tabs_means_no_arrow_hijack() {
    tui::CommandLine cl;
    cl.set_completions({"one", "two"}, "/set ");
    cl.on_char('x');
    cl.on_left();
    // No provider tabs configured: the cursor moved and nothing else did.
    ASSERT_EQ(cl.cursor(), cl.text().size() - 1);
    return 0;
}

// ── Test: Ctrl-A / Ctrl-E ──
static int test_ctrl_a_e() {
    tui::CommandLine cl;
    cl.set_text("hello world");
    cl.on_ctrl_a();
    ASSERT_EQ(cl.cursor(), 0);
    cl.on_ctrl_e();
    ASSERT_EQ(cl.cursor(), 11);
    return 0;
}

// ── Test: Ctrl-W delete word backward ──
static int test_ctrl_w() {
    tui::CommandLine cl;
    cl.set_text("/set detection loop");
    // Move cursor to end, then delete last word.
    cl.on_ctrl_e();
    cl.on_ctrl_w();
    ASSERT_EQ(cl.text(), "/set detection ");
    return 0;
}

// ── Test: Ctrl-U delete to start ──
static int test_ctrl_u() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("some text here", 9); // cursor after "some text"
    cl.on_ctrl_u();
    ASSERT_EQ(cl.text(), " here");
    ASSERT_EQ(cl.cursor(), 0);
    return 0;
}

// ── Test: Ctrl-K delete to end ──
static int test_ctrl_k() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("some text here", 5); // cursor after "some "
    cl.on_ctrl_k();
    ASSERT_EQ(cl.text(), "some ");
    ASSERT_EQ(cl.cursor(), 5);
    return 0;
}

// ── Test: clearing the line closes the drawer with it ──
//
// The drawer flag and the text are one piece of state: a command line with no
// text cannot open a drawer. Ctrl-U used to leave the flag set, so the renderer
// went on to parse an empty line as a command ("".substr(1)) and threw
// out_of_range from the idle tick, killing the TUI with no warning.
static int test_ctrl_u_closes_an_open_drawer() {
    tui::CommandLine cl;
    cl.on_char('/');
    cl.on_char('s');
    ASSERT(cl.drawer_open());
    cl.on_ctrl_u();
    ASSERT_EQ(cl.text(), "");
    ASSERT(!cl.drawer_open());
    return 0;
}

static int test_ctrl_k_closes_an_open_drawer() {
    tui::CommandLine cl;
    cl.on_char('/');
    cl.on_char('s');
    ASSERT(cl.drawer_open());
    cl.on_ctrl_k(); // cursor is at the end, so nothing is deleted yet
    ASSERT(cl.drawer_open());
    cl.on_ctrl_a();
    cl.on_ctrl_k(); // now the whole "/s" goes
    ASSERT_EQ(cl.text(), "");
    ASSERT(!cl.drawer_open());
    return 0;
}

// ── Test: undo ──
static int test_undo() {
    tui::CommandLine cl;
    cl.set_text("hello");
    cl.on_ctrl_w(); // delete word backward — saves undo
    ASSERT(cl.text().empty());
    cl.on_undo();
    ASSERT_EQ(cl.text(), "hello");
    return 0;
}

// ── Test: ? without space → ShowPopup ──
static int test_question_no_space() {
    tui::CommandLine cl;
    cl.set_text("/set"); // cursor at end: /set|
    // Insert '?' — no space before it.
    auto r = cl.on_char('?');
    ASSERT_EQ(r.action, tui::CommandLine::Result::ShowPopup);
    // Input should be restored to without the ?.
    ASSERT_EQ(cl.text(), "/set");
    return 0;
}

// ── Test: ? with space → ShowHelpPage ──
static int test_question_with_space() {
    tui::CommandLine cl;
    cl.set_text("/set "); // trailing space: /set |
    auto r = cl.on_char('?');
    ASSERT_EQ(r.action, tui::CommandLine::Result::ShowHelpPage);
    ASSERT_EQ(r.help_node, "/set");
    // Input restored to /set (without trailing space)
    ASSERT_EQ(cl.text(), "/set ");
    return 0;
}

// ── Test: ? with space at nested depth ──
static int test_question_nested() {
    tui::CommandLine cl;
    cl.set_text("/set detection "); // /set detection |
    auto r = cl.on_char('?');
    ASSERT_EQ(r.action, tui::CommandLine::Result::ShowHelpPage);
    ASSERT_EQ(r.help_node, "/set detection");
    // Input restored
    ASSERT_EQ(cl.text(), "/set detection ");
    return 0;
}

// ── Test: @ triggers ShowPopup ──
static int test_at_reference() {
    tui::CommandLine cl;
    cl.set_text("check this file ");
    auto r = cl.on_char('@');
    ASSERT_EQ(r.action, tui::CommandLine::Result::ShowPopup);
    ASSERT_EQ(cl.text(), "check this file @");
    return 0;
}

// ── Test: Enter dispatches text ──
static int test_enter_dispatch() {
    tui::CommandLine cl;
    cl.set_text("hello");
    auto r = cl.on_enter();
    ASSERT_EQ(r.action, tui::CommandLine::Result::Dispatch);
    ASSERT_EQ(r.dispatch_text, "hello");
    // Input cleared
    ASSERT(cl.text().empty());
    // History updated
    ASSERT_EQ(cl.history().size(), 1u);
    ASSERT_EQ(cl.history()[0], "hello");
    return 0;
}

// ── Test: Up/Down history navigation ──
static int test_history_nav() {
    tui::CommandLine cl;
    cl.set_history({"first", "second", "third"});
    cl.on_up();
    ASSERT_EQ(cl.text(), "third");
    cl.on_up();
    ASSERT_EQ(cl.text(), "second");
    cl.on_down();
    ASSERT_EQ(cl.text(), "third");
    return 0;
}

// ── Test: Tab recomputes shadow ──
static int test_tab_updates_state() {
    tui::CommandLine cl;
    cl.set_text("/se");
    auto r = cl.on_tab();
    // Tab should accept shadow or at minimum not crash.
    ASSERT_EQ(r.action, tui::CommandLine::Result::None);
    return 0;
}

// ── Test: @ followed by normal chars ──
static int test_at_then_normal() {
    tui::CommandLine cl;
    cl.set_text("read ");
    cl.on_char('@');
    cl.on_char('f');
    cl.on_char('i');
    cl.on_char('l');
    ASSERT_EQ(cl.text(), "read @fil");
    return 0;
}

// ── Test: long input doesn't crash ──
static int test_long_input() {
    tui::CommandLine cl;
    std::string s;
    for (int i = 0; i < 1000; ++i)
        s += 'x';
    cl.set_text(s);
    ASSERT_EQ(cl.text().size(), 1000u);
    cl.on_ctrl_a();
    ASSERT_EQ(cl.cursor(), 0u);
    cl.on_ctrl_e();
    ASSERT_EQ(cl.cursor(), 1000u);
    return 0;
}

// ── Test: empty input ──
static int test_empty_input() {
    tui::CommandLine cl;
    ASSERT(cl.text().empty());
    ASSERT_EQ(cl.cursor(), 0u);
    ASSERT(!cl.drawer_open());
    auto r = cl.on_enter();
    ASSERT_EQ(r.action, tui::CommandLine::Result::None); // empty = no dispatch
    return 0;
}

// ── Test: Ctrl-Y yank after Ctrl-W ──
static int test_yank() {
    tui::CommandLine cl;
    cl.set_text("hello world");
    cl.on_ctrl_e();
    cl.on_ctrl_w(); // deletes "world"
    ASSERT_EQ(cl.text(), "hello ");
    cl.on_ctrl_y(); // yanks "world" back
    ASSERT_EQ(cl.text(), "hello world");
    return 0;
}

// ── Test: drawer Enter dispatch preserves the typed prefix ──
// "/set model llama" + Enter on a drawer row must become
// "/set model llama3-8b", never "/llama3-8b" (the completion replaces only
// the trailing partial token).
static int test_enter_drawer_preserves_prefix() {
    tui::CommandLine cl;
    cl.set_text("/set model llama");
    cl.set_completions({"llama3-8b", "llama2-7b"});
    auto r = cl.on_enter();
    ASSERT_EQ(r.action, tui::CommandLine::Result::Dispatch);
    ASSERT_EQ(r.dispatch_text, "/set model llama3-8b");
    return 0;
}

// ── Test: Tab does not append when the partial matches no completion ──
// "/set model" must not become "/set modelllama3-8b" when no model id
// starts with "model".
static int test_tab_no_match_no_append() {
    tui::CommandLine cl;
    cl.set_text("/set model");
    cl.set_completions({"llama3-8b"});
    auto r = cl.on_tab();
    ASSERT_EQ(r.action, tui::CommandLine::Result::None);
    ASSERT_EQ(cl.text(), "/set model");
    return 0;
}

// ── Test: Tab appends for dotted suffixes (namespace leaf completion) ──
static int test_tab_dotted_suffix_appends() {
    tui::CommandLine cl;
    cl.set_text("/set detection.");
    cl.set_completions({"loop", "duplicate"});
    cl.on_tab();
    ASSERT_EQ(cl.text(), "/set detection.loop");
    return 0;
}

// ── Test: shadow with dotted namespace + leaf partial (real flow) ──
static int test_shadow_dotted_partial() {
    tui::CommandLine cl;
    cl.set_completions({"deepseek-chat", "deepseek-reasoner"});
    cl.set_text("/set model d");
    ASSERT_EQ(cl.shadow(), "eepseek-chat");
    return 0;
}

// ── Test: shadow must never be a single letter for a multi-char word ──
static int test_shadow_never_single_letter() {
    tui::CommandLine cl;
    cl.set_completions({"policy", "provider"});
    cl.set_text("/set p");
    std::string s = cl.shadow();
    ASSERT(s.size() > 1);
    ASSERT_EQ(s, "olicy");
    return 0;
}

// ── Test: tab cycle replaces only the partial token ──
static int test_tab_cycle_replaces_only_token() {
    tui::CommandLine cl;
    cl.set_completions({"policy", "provider"});
    cl.set_text("/set p");
    cl.on_tab(); // accept shadow → /set policy
    ASSERT_EQ(cl.text(), "/set policy");
    cl.on_tab(); // cycle → provider
    ASSERT_EQ(cl.text(), "/set provider");
    cl.on_tab(); // cycle back → policy
    ASSERT_EQ(cl.text(), "/set policy");
    return 0;
}

// Enter runs the command the user actually typed when its trailing token is an
// exact drawer row. Before this, an open drawer always ran the highlighted row,
// so "/window list" + Enter could run "/window attach" when the keys arrived
// with a pause between them (a real terminal) instead of in one burst (a pty).
static int test_enter_prefers_the_exact_typed_row() {
    tui::CommandLine cl;
    cl.set_completions({"attach", "list", "new"});
    cl.set_text("/window list");
    auto r = cl.on_enter();
    ASSERT(r.action == tui::CommandLine::Result::Dispatch);
    ASSERT_EQ(r.dispatch_text, std::string("/window list"));
    ASSERT_EQ(cl.text(), std::string());
    return 0;
}

// ── Test: enter dispatches the drawer row (descend case, empty partial) ──
static int test_enter_dispatches_drawer_row_descend() {
    tui::CommandLine cl;
    cl.set_completions({"approval", "mode", "rule", "timeout"});
    cl.set_text("/set policy ");
    auto r = cl.on_enter();
    ASSERT(r.action == tui::CommandLine::Result::Dispatch);
    ASSERT_EQ(r.dispatch_text, "/set policy approval");
    return 0;
}

// ── Tests: multi-line input ──

static int test_newline_inserts_a_line_break() {
    tui::CommandLine cl;
    cl.on_char('a');
    cl.on_newline();
    cl.on_char('b');
    ASSERT_EQ(cl.text(), std::string("a\nb"));
    ASSERT_EQ(cl.cursor(), 3u);
    ASSERT_EQ(cl.line_count(), 2);
    return 0;
}

static int test_newline_is_ignored_when_multiline_is_off() {
    tui::CommandLine cl;
    cl.set_multiline(false);
    cl.on_char('a');
    cl.on_newline();
    ASSERT_EQ(cl.text(), std::string("a"));
    ASSERT_EQ(cl.line_count(), 1);
    return 0;
}

static int test_up_moves_within_the_input_then_reaches_history() {
    tui::CommandLine cl;
    cl.set_history({"old"});
    cl.set_text("abc\ndef");
    cl.on_up(); // same column, previous line
    ASSERT_EQ(cl.cursor(), 3u);
    ASSERT_EQ(cl.text(), std::string("abc\ndef"));
    cl.on_up(); // already on the first line: history
    ASSERT_EQ(cl.text(), std::string("old"));
    return 0;
}

static int test_down_moves_within_the_input() {
    tui::CommandLine cl;
    cl.set_text("abc\ndef");
    cl.set_text_and_cursor(cl.text(), 3); // end of the first line
    cl.on_down();
    ASSERT_EQ(cl.cursor(), 7u);
    ASSERT_EQ(cl.text(), std::string("abc\ndef"));
    cl.on_down(); // last line and no history: nothing to move to
    ASSERT_EQ(cl.text(), std::string("abc\ndef"));
    ASSERT_EQ(cl.cursor(), 7u);
    return 0;
}

static int test_vertical_movement_clamps_to_a_shorter_line() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("abcdef\nxy", 6); // end of the long first line
    cl.on_down();                            // the short line can only take column 2
    ASSERT_EQ(cl.cursor(), 9u);
    cl.on_up();
    ASSERT_EQ(cl.cursor(), 2u); // the return trip keeps the clamped column
    return 0;
}

static int test_ctrl_a_e_use_the_current_line() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("abc\ndef", 5);
    cl.on_ctrl_a();
    ASSERT_EQ(cl.cursor(), 4u);
    cl.on_ctrl_e();
    ASSERT_EQ(cl.cursor(), 7u);
    return 0;
}

static int test_home_end_use_the_current_line() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("abc\ndef", 5);
    cl.on_home();
    ASSERT_EQ(cl.cursor(), 4u);
    cl.on_end();
    ASSERT_EQ(cl.cursor(), 7u);
    return 0;
}

static int test_backspace_joins_lines() {
    tui::CommandLine cl;
    cl.set_text_and_cursor("ab\ncd", 3);
    cl.on_backspace();
    ASSERT_EQ(cl.text(), std::string("abcd"));
    ASSERT_EQ(cl.cursor(), 2u);
    return 0;
}

static int test_paste_keeps_newlines_when_multiline_is_on() {
    tui::CommandLine cl;
    cl.on_paste("a\r\nb");
    ASSERT_EQ(cl.text(), std::string("a\nb"));
    return 0;
}

static int test_paste_flattens_newlines_when_multiline_is_off() {
    tui::CommandLine cl;
    cl.set_multiline(false);
    cl.on_paste("a\r\nb");
    ASSERT_EQ(cl.text(), std::string("a b"));
    return 0;
}

// ── Tests: draft persistence ──

static std::string draft_temp_root(const char* tag) {
    namespace fs = std::filesystem;
    const fs::path p =
        fs::temp_directory_path() / (std::string("amber_draft_") + tag + "_" +
                                     std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

static int test_draft_round_trips_through_the_workspace_file() {
    const std::string root = draft_temp_root("roundtrip");
    ASSERT_EQ(tui::draft_store::path(root), root + "/.amber/draft");
    ASSERT(tui::draft_store::save(root, "hello\nworld"));
    ASSERT_EQ(tui::draft_store::load(root), std::string("hello\nworld"));
    tui::draft_store::clear(root);
    ASSERT_EQ(tui::draft_store::load(root), std::string(""));
    std::filesystem::remove_all(root);
    return 0;
}

static int test_a_missing_or_unreadable_draft_reads_as_empty() {
    namespace fs = std::filesystem;
    const std::string root = draft_temp_root("unreadable");
    ASSERT_EQ(tui::draft_store::load(root), std::string(""));
    fs::create_directories(tui::draft_store::path(root)); // a directory cannot be read
    ASSERT_EQ(tui::draft_store::load(root), std::string(""));
    fs::remove_all(root);
    return 0;
}

static int test_the_draft_keeper_writes_after_the_debounce() {
    const std::string root = draft_temp_root("debounce");
    tui::draft_store::DraftKeeper keeper;
    const auto start = tui::draft_store::DraftKeeper::Clock::now();
    keeper.seed(root, "");

    keeper.update(root, "hello", start);
    ASSERT_EQ(tui::draft_store::load(root), std::string("")); // not due yet
    keeper.update(root, "hello", start + std::chrono::milliseconds(400));
    ASSERT_EQ(tui::draft_store::load(root), std::string(""));
    keeper.update(root, "hello", start + std::chrono::milliseconds(500));
    ASSERT_EQ(tui::draft_store::load(root), std::string("hello"));

    keeper.clear();
    ASSERT_EQ(tui::draft_store::load(root), std::string(""));
    std::filesystem::remove_all(root);
    return 0;
}

static int test_a_new_keystroke_restarts_the_debounce() {
    const std::string root = draft_temp_root("restart");
    tui::draft_store::DraftKeeper keeper;
    const auto start = tui::draft_store::DraftKeeper::Clock::now();
    keeper.seed(root, "");
    keeper.update(root, "he", start);
    keeper.update(root, "hel", start + std::chrono::milliseconds(400));
    keeper.update(root, "hel", start + std::chrono::milliseconds(600));
    ASSERT_EQ(tui::draft_store::load(root), std::string("")); // 600 < 900
    keeper.update(root, "hel", start + std::chrono::milliseconds(900));
    ASSERT_EQ(tui::draft_store::load(root), std::string("hel"));
    tui::draft_store::clear(root);
    std::filesystem::remove_all(root);
    return 0;
}

int main() {
    int failed = 0;
    failed += run_test("newline inserts a line break", test_newline_inserts_a_line_break);
    failed += run_test("newline ignored when multiline is off",
                       test_newline_is_ignored_when_multiline_is_off);
    failed += run_test("up moves within the input then reaches history",
                       test_up_moves_within_the_input_then_reaches_history);
    failed += run_test("down moves within the input", test_down_moves_within_the_input);
    failed += run_test("vertical movement clamps to a shorter line",
                       test_vertical_movement_clamps_to_a_shorter_line);
    failed += run_test("Ctrl-A/E use the current line", test_ctrl_a_e_use_the_current_line);
    failed += run_test("Home/End use the current line", test_home_end_use_the_current_line);
    failed += run_test("backspace joins lines", test_backspace_joins_lines);
    failed += run_test("paste keeps newlines when multiline is on",
                       test_paste_keeps_newlines_when_multiline_is_on);
    failed += run_test("paste flattens newlines when multiline is off",
                       test_paste_flattens_newlines_when_multiline_is_off);
    failed += run_test("draft round trips through the workspace file",
                       test_draft_round_trips_through_the_workspace_file);
    failed += run_test("a missing or unreadable draft reads as empty",
                       test_a_missing_or_unreadable_draft_reads_as_empty);
    failed += run_test("the draft keeper writes after the debounce",
                       test_the_draft_keeper_writes_after_the_debounce);
    failed += run_test("a new keystroke restarts the debounce",
                       test_a_new_keystroke_restarts_the_debounce);
    failed += run_test("basic insertion", test_basic_insertion);
    failed += run_test("slash triggers drawer", test_slash_triggers_drawer);
    failed += run_test("left moves tabs when the drawer is open",
                       test_left_moves_tabs_when_the_drawer_is_open);
    failed += run_test("left at the leftmost tab does nothing",
                       test_left_at_the_leftmost_tab_does_nothing);
    failed += run_test("right moves towards a provider", test_right_moves_towards_a_provider);
    failed += run_test("tab rows are filtered to the selected provider",
                       test_tab_rows_are_filtered_to_the_selected_provider);
    failed += run_test("enter dispatches from the selected tab",
                       test_enter_dispatches_from_the_selected_tab);
    failed += run_test("tab selection survives reseeding by name",
                       test_tab_selection_survives_reseeding_by_name);
    failed += run_test("arrows return to cursor movement when the drawer is closed",
                       test_arrows_return_to_cursor_movement_when_the_drawer_is_closed);
    failed += run_test("up down move the row within a tab", test_up_down_move_the_row_within_a_tab);
    failed += run_test("reopening the drawer restores the tab",
                       test_reopening_the_drawer_restores_the_tab);
    failed += run_test("no tabs means no arrow hijack", test_no_tabs_means_no_arrow_hijack);
    failed += run_test("backspace", test_backspace);
    failed += run_test("Ctrl-A/E", test_ctrl_a_e);
    failed += run_test("Ctrl-W delete word", test_ctrl_w);
    failed += run_test("Ctrl-U delete to start", test_ctrl_u);
    failed += run_test("Ctrl-K delete to end", test_ctrl_k);
    failed += run_test("Ctrl-U closes the drawer", test_ctrl_u_closes_an_open_drawer);
    failed += run_test("Ctrl-K closes the drawer", test_ctrl_k_closes_an_open_drawer);
    failed += run_test("undo", test_undo);
    failed += run_test("? without space → popup", test_question_no_space);
    failed += run_test("? with space → help page", test_question_with_space);
    failed += run_test("? nested path", test_question_nested);
    failed += run_test("@ reference", test_at_reference);
    failed += run_test("enter dispatches", test_enter_dispatch);
    failed += run_test("history navigation", test_history_nav);
    failed += run_test("tab updates state", test_tab_updates_state);
    failed += run_test("@ then normal chars", test_at_then_normal);
    failed += run_test("long input", test_long_input);
    failed += run_test("empty input", test_empty_input);
    failed += run_test("Ctrl-Y yank", test_yank);
    failed += run_test("drawer enter preserves prefix", test_enter_drawer_preserves_prefix);
    failed += run_test("tab no-match no append", test_tab_no_match_no_append);
    failed += run_test("tab dotted suffix appends", test_tab_dotted_suffix_appends);
    failed += run_test("shadow dotted partial", test_shadow_dotted_partial);
    failed += run_test("shadow never single letter", test_shadow_never_single_letter);
    failed += run_test("tab cycle replaces only token", test_tab_cycle_replaces_only_token);
    failed +=
        run_test("enter dispatches drawer row descend", test_enter_dispatches_drawer_row_descend);
    failed += run_test("enter prefers the exact typed row", test_enter_prefers_the_exact_typed_row);

    std::cout << "\n" << (failed ? "FAILED" : "ALL PASSED") << " (" << failed << " failures)\n";
    return failed;
}
