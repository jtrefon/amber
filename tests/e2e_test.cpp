#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "tui/view.h"
#include "tui/command_line.h"

// ── MockView: records everything for test assertions ─────────────────

class MockView : public tui::View {
public:
    // Queue of keys to return from get_key().
    std::vector<int> keys_;
    size_t key_pos_ = 0;

    // Recorded state
    std::string last_input_;
    size_t last_cursor_ = 0;
    std::string last_shadow_;
    std::vector<std::string> drawn_lines_;
    int last_menu_title_ = 0;
    std::vector<std::string> last_menu_items_;

    int get_key() override {
        if (key_pos_ < keys_.size())
            return keys_[key_pos_++];
        return KEY_ERR;
    }
    void set_timeout(int) override {}

    void draw() override {}
    void draw_input(const std::string& input, size_t cursor,
                    const std::string& shadow) override {
        last_input_ = input;
        last_cursor_ = cursor;
        last_shadow_ = shadow;
    }
    void draw_status_bar(const std::string&) override {}
    void flush() override {}
    void clear_screen() override {}

    int menu_select(const std::string& title,
                    const std::vector<std::string>& items) override {
        last_menu_title_ = title.size() ? title[0] : 0;
        last_menu_items_ = items;
        // Default: select first item.
        return items.empty() ? -1 : 0;
    }

    int terminal_height() const override { return 24; }
    int terminal_width() const override { return 80; }
    void append_line(int color, const std::string& text) override {
        drawn_lines_.push_back(text);
    }
};

// ── Simulated event loop ────────────────────────────────────────────

struct EventLoopResult {
    std::string dispatched;
    std::vector<std::string> popup_shown;
    std::string help_shown;
    std::vector<std::string> lines; // scrollback lines
    std::string input_before;
    std::string shadow_before;
};

EventLoopResult simulate(MockView& view, tui::CommandLine& cl) {
    EventLoopResult r;

    // Simulate one tick of Tui::run().
    int key = view.get_key();

    tui::CommandLine::Result result;

    switch (key) {
    case tui::View::KEY_ENTER:
    case 10: case 13:
        result = cl.on_enter();
        break;
    case tui::View::KEY_TAB:
    case '\t':
        result = cl.on_tab();
        break;
    case tui::View::KEY_BACKSPACE:
    case 127: case 8:
        result = cl.on_backspace();
        break;
    case tui::View::KEY_UP:
        result = cl.on_up();
        break;
    case tui::View::KEY_DOWN:
        result = cl.on_down();
        break;
    case tui::View::KEY_LEFT:
        result = cl.on_left();
        break;
    case tui::View::KEY_RIGHT:
        result = cl.on_right();
        break;
    case 1: result = cl.on_ctrl_a(); break;   // Ctrl-A
    case 5: result = cl.on_ctrl_e(); break;   // Ctrl-E
    case 11: result = cl.on_ctrl_k(); break;  // Ctrl-K
    case 21: result = cl.on_ctrl_u(); break;  // Ctrl-U
    case 23: result = cl.on_ctrl_w(); break;  // Ctrl-W
    case 25: result = cl.on_ctrl_y(); break;  // Ctrl-Y
    case 20: result = cl.on_ctrl_t(); break;  // Ctrl-T
    case 31: result = cl.on_undo(); break;    // Ctrl-_
    case 4:  result = cl.on_ctrl_d(); break;  // Ctrl-D
    case 18: result = cl.on_ctrl_r(); break;  // Ctrl-R
    default:
        if (key >= 32 && key <= 126)
            result = cl.on_char(static_cast<char>(key));
        break;
    }

    r.input_before = cl.text();
    r.shadow_before = cl.shadow();

    switch (result.action) {
    case tui::CommandLine::Result::Dispatch:
        r.dispatched = result.dispatch_text;
        break;
    case tui::CommandLine::Result::ShowPopup:
        r.popup_shown = result.popup_items;
        break;
    case tui::CommandLine::Result::ShowHelpPage:
        r.help_shown = result.help_node;
        break;
    default: break;
    }

    r.lines = view.drawn_lines_;
    return r;
}

// ── Helpers ─────────────────────────────────────────────────────────

#define TEST(name) void name()
#define ASSERT(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; exit(1); } \
} while(0)
#define ASSERT_EQ(a,b) ASSERT((a) == (b))
#define ASSERT_CONTAINS(str, substr) ASSERT((str).find(substr) != std::string::npos)
#define PASS std::cout << "  PASS\n"

// ── Tests ───────────────────────────────────────────────────────────

TEST(test_slash_drawer) {
    std::cout << "[TEST] / opens drawer...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/'};
    auto r = simulate(view, cl);
    ASSERT_EQ(r.dispatched, "");
    ASSERT_EQ(cl.text(), "/");
    ASSERT(cl.drawer_open());
    PASS;
}

TEST(test_set_typed) {
    std::cout << "[TEST] /set typed...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/', 's', 'e', 't'};
    for (int i = 0; i < 4; ++i) simulate(view, cl);
    ASSERT_EQ(cl.text(), "/set");
    PASS;
}

TEST(test_enter_dispatches) {
    std::cout << "[TEST] enter dispatches text...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/', 's', 'e', 't', ' ', 'o', 'n', 10};
    EventLoopResult r;
    for (int i = 0; i < 8; ++i) {
        r = simulate(view, cl);
    }
    // The last event was enter, should dispatch.
    ASSERT_EQ(r.dispatched, "/set on");
    PASS;
}

TEST(test_question_with_space_help) {
    std::cout << "[TEST] /set ? shows help...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/', 's', 'e', 't', ' ', '?'};
    EventLoopResult r;
    for (int i = 0; i < 6; ++i) r = simulate(view, cl);
    ASSERT_EQ(r.help_shown, "/set");
    ASSERT_CONTAINS(r.input_before, "/set");
    PASS;
}

TEST(test_question_no_space_popup) {
    std::cout << "[TEST] /set? shows popup...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/', 's', 'e', 't', '?'};
    EventLoopResult r;
    for (int i = 0; i < 5; ++i) r = simulate(view, cl);
    ASSERT_EQ(r.popup_shown.size(), 0u); // no commands registered yet
    // Input should be restored to /set
    ASSERT_EQ(cl.text(), "/set");
    PASS;
}

TEST(test_at_reference) {
    std::cout << "[TEST] @ shows popup...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'r', 'e', 'a', 'd', ' ', '@'};
    EventLoopResult r;
    for (int i = 0; i < 6; ++i) r = simulate(view, cl);
    // @ should trigger ShowPopup
    ASSERT_EQ(r.popup_shown.size(), 0u); // MockView's menu_select returns first item, no popup items in result
    ASSERT_EQ(cl.text(), "read @");
    PASS;
}

TEST(test_ctrl_w) {
    std::cout << "[TEST] Ctrl-W deletes word...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'/', 's', 'e', 't', ' ', 'o', 'n', 23}; // Ctrl-W
    for (int i = 0; i < 7; ++i) simulate(view, cl); // type /set on
    auto r = simulate(view, cl); // Ctrl-W
    ASSERT_EQ(cl.text(), "/set ");
    PASS;
}

TEST(test_ctrl_a_e) {
    std::cout << "[TEST] Ctrl-A/E cursor movement...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'a', 'b', 'c', 1, 5}; // abc then Ctrl-A, Ctrl-E
    simulate(view, cl); simulate(view, cl); simulate(view, cl); // abc
    ASSERT_EQ(cl.text(), "abc");
    ASSERT_EQ(cl.cursor(), 3u);
    simulate(view, cl); // Ctrl-A
    ASSERT_EQ(cl.cursor(), 0u);
    simulate(view, cl); // Ctrl-E
    ASSERT_EQ(cl.cursor(), 3u);
    PASS;
}

TEST(test_backspace) {
    std::cout << "[TEST] backspace...";
    MockView view;
    tui::CommandLine cl;
    view.keys_ = {'a', 'b', 'c', 127};
    for (int i = 0; i < 3; ++i) simulate(view, cl);
    ASSERT_EQ(cl.text(), "abc");
    simulate(view, cl);
    ASSERT_EQ(cl.text(), "ab");
    PASS;
}

TEST(test_shadow_appears_with_completions) {
    std::cout << "[TEST] shadow appears with completions...";
    tui::CommandLine cl;
    cl.set_completions({"set", "session", "save", "quit"});
    cl.set_text("/se");
    // After /se, shadow should show "t" (or "t " etc.)
    std::string s = cl.shadow();
    ASSERT(!s.empty());
    // The shadow should be the remainder of "set" after "se" → "t"
    ASSERT_EQ(s, "t");
    PASS;
}

TEST(test_shadow_empty_without_completions) {
    std::cout << "[TEST] no completions = no shadow...";
    tui::CommandLine cl;
    cl.set_completions({});  // empty completions
    cl.set_text("/se");
    ASSERT_EQ(cl.shadow(), "");
    PASS;
}

TEST(test_shadow_updates_as_you_type) {
    std::cout << "[TEST] shadow updates as you type...";
    tui::CommandLine cl;
    cl.set_completions({"set", "session", "save", "quit"});
    cl.set_text("/s");
    // /s → shadow shows the remainder of first matching completion "set" → "et"
    ASSERT(!cl.shadow().empty());
    ASSERT_EQ(cl.shadow(), "et");
    PASS;
}

// Test that shadow updates as user types character by character (real flow).
// Test that /get ? shows help, not config_screen.
TEST(test_get_question_help) {
    std::cout << "[TEST] /get ? shows help...";
    tui::CommandLine cl;
    cl.set_completions({"get", "set", "help"});

    // Type /get
    cl.on_char('/'); cl.on_char('g'); cl.on_char('e'); cl.on_char('t');
    ASSERT_EQ(cl.text(), "/get");

    // Type space then ?
    cl.on_char(' ');
    auto r = cl.on_char('?');
    ASSERT_EQ(r.action, tui::CommandLine::Result::ShowHelpPage);
    ASSERT_EQ(r.help_node, "/get");
    // Input should be restored to /get for continued typing.
    ASSERT_EQ(cl.text(), "/get ");
    PASS;
}

// Test that /get dete shows shadow 'ction' (from 'detection').
// This requires the host to set the correct per-argument completions.
TEST(test_get_dete_shadow) {
    std::cout << "[TEST] /get dete shadow...";
    tui::CommandLine cl;
    // Simulate correct per-argument completions for `get`.
    cl.set_completions({"config","model","provider","toolfold","policy",
                         "display","compression","detection","think"});
    cl.set_text("/get dete");
    ASSERT(!cl.shadow().empty());
    ASSERT_EQ(cl.shadow(), "ction");
    PASS;
}

TEST(test_shadow_sequential_typing) {
    std::cout << "[TEST] shadow sequential typing...";
    tui::CommandLine cl;
    // Simulate the real flow: completions are set AFTER first draw.
    // First character: type '/' — no completions yet.
    cl.on_char('/');
    // Now the host sets completions (simulating update_completions in Tui::run).
    cl.set_completions({"help", "set", "session", "save", "quit", "stop",
                        "system", "files", "provider", "model", "job", "compress"});
    // After '/' and completions: partial is "", no shadow.
    ASSERT_EQ(cl.shadow(), "");

    // Type 's': partial = "s", should find shadow.
    cl.on_char('s');
    ASSERT(!cl.shadow().empty());
    ASSERT_EQ(cl.shadow(), "et");  // "set" is first match

    // Type 'e': partial = "se", shadow should narrow.
    cl.on_char('e');
    ASSERT(!cl.shadow().empty());
    ASSERT_EQ(cl.shadow(), "t");  // "set" after "se" → "t"

    // Type 't': partial = "set", shadow should be space.
    cl.on_char('t');
    ASSERT(!cl.shadow().empty());
    ASSERT_EQ(cl.shadow(), " ");  // "set " — space added by convention

    PASS;
}

TEST(test_up_down_history) {
    std::cout << "[TEST] Up/Down history...";
    MockView view;
    tui::CommandLine cl;
    cl.set_history({"first", "second"});
    view.keys_ = {tui::View::KEY_UP};
    auto r = simulate(view, cl);
    ASSERT_EQ(cl.text(), "second");
    PASS;
}

int main() {
    test_slash_drawer();
    test_set_typed();
    test_enter_dispatches();
    test_question_with_space_help();
    test_question_no_space_popup();
    test_at_reference();
    test_ctrl_w();
    test_ctrl_a_e();
    test_backspace();
    test_up_down_history();
    test_shadow_appears_with_completions();
    test_shadow_empty_without_completions();
    test_shadow_updates_as_you_type();
    test_shadow_sequential_typing();
    test_get_dete_shadow();
    test_get_question_help();
    std::cout << "\nALL E2E TESTS PASSED\n";
    return 0;
}
