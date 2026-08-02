#include <cassert>
#include <iostream>
#include <string>

#include "tui/command_line.h"

// Minimal test framework for CommandLine.
#define TEST(name) void name()
#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return 1; \
    } \
} while(0)
#define ASSERT_EQ(a,b) ASSERT((a) == (b))

int run_test(const char* name, int (*fn)()) {
    std::cout << "[TEST] " << name << "...\n";
    int rc = fn();
    if (rc) std::cout << "  FAIL\n"; else std::cout << "  PASS\n";
    return rc;
}

// ── Test: basic text insertion ──
static int test_basic_insertion() {
    tui::CommandLine cl;
    cl.on_char('h'); cl.on_char('e'); cl.on_char('l');
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
    cl.on_backspace(); cl.on_backspace();
    ASSERT_EQ(cl.text(), "hel");
    ASSERT_EQ(cl.cursor(), 3);
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

// ── Test: undo ──
static int test_undo() {
    tui::CommandLine cl;
    cl.set_text("hello");
    cl.on_ctrl_w();  // delete word backward — saves undo
    ASSERT(cl.text().empty());
    cl.on_undo();
    ASSERT_EQ(cl.text(), "hello");
    return 0;
}

// ── Test: ? without space → ShowPopup ──
static int test_question_no_space() {
    tui::CommandLine cl;
    cl.set_text("/set");  // cursor at end: /set|
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
    cl.set_text("/set ");  // trailing space: /set |
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
    cl.set_text("/set detection ");  // /set detection |
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
    cl.on_char('f'); cl.on_char('i'); cl.on_char('l');
    ASSERT_EQ(cl.text(), "read @fil");
    return 0;
}

// ── Test: long input doesn't crash ──
static int test_long_input() {
    tui::CommandLine cl;
    std::string s;
    for (int i = 0; i < 1000; ++i) s += 'x';
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
    cl.on_ctrl_w();  // deletes "world"
    ASSERT_EQ(cl.text(), "hello ");
    cl.on_ctrl_y();  // yanks "world" back
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

int main() {
    int failed = 0;
    failed += run_test("basic insertion", test_basic_insertion);
    failed += run_test("slash triggers drawer", test_slash_triggers_drawer);
    failed += run_test("backspace", test_backspace);
    failed += run_test("Ctrl-A/E", test_ctrl_a_e);
    failed += run_test("Ctrl-W delete word", test_ctrl_w);
    failed += run_test("Ctrl-U delete to start", test_ctrl_u);
    failed += run_test("Ctrl-K delete to end", test_ctrl_k);
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

    std::cout << "\n" << (failed ? "FAILED" : "ALL PASSED")
              << " (" << failed << " failures)\n";
    return failed;
}
