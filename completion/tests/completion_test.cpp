// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "completion/command.h"
#include "completion/completer.h"
#include "completion/filter.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Minimal test harness (same pattern as tests/test_util.h)
namespace {
struct Case {
    std::string name;
    void (*fn)();
};
std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
int failures = 0;
int passed = 0;
struct Registrar {
    Registrar(const std::string& name, void (*fn)()) {
        registry().push_back({name, fn});
    }
};
[[noreturn]] void fail(const std::string& msg) { throw msg; }
int run_all() {
    for (const auto& c : registry()) {
        try { c.fn(); ++passed; std::cout << "[ PASS ] " << c.name << "\n"; }
        catch (const std::string& msg) { ++failures; std::cout << "[ FAIL ] " << c.name << ": " << msg << "\n"; }
        catch (const std::exception& e) { ++failures; std::cout << "[ FAIL ] " << c.name << ": " << e.what() << "\n"; }
        catch (...) { ++failures; std::cout << "[ FAIL ] " << c.name << ": unknown\n"; }
    }
    std::cout << "\n" << passed << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
} // namespace

#define TEST(name) \
    static void test_##name(); \
    static ::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define ASSERT(cond) do { if (!(cond)) { \
    std::ostringstream _os; _os << "assert: " #cond << " (" << __FILE__ << ":" << __LINE__ << ")"; \
    ::fail(_os.str()); \
} } while(0)

#define ASSERT_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        std::ostringstream _os; _os << "eq: " #a " == " #b " (" << __FILE__ << ":" << __LINE__ << ") " \
            << _va << " != " << _vb; \
        ::fail(_os.str()); \
    } \
} while(0)
#define ASSERT_TRUE(cond) ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

using namespace completion;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Build a command tree matching the TUI slash commands.
static std::vector<std::unique_ptr<Command>> make_commands() {
    std::vector<std::unique_ptr<Command>> cmds;

    // /set
    auto set = std::make_unique<Command>();
    set->name = "set";
    set->description = "Configure runtime settings";
    set->args_usage = "<setting> <value>";

    // detection subcommand
    auto det = std::make_unique<Command>();
    det->name = "detection";
    det->description = "Configure detection subsystems";
    auto det_loop = std::make_unique<Command>();
    det_loop->name = "loop";
    det_loop->description = "Tool/text loop detection";
    det_loop->args_usage = "<on|off|toggle>";
    det_loop->args.push_back({"value", "on/off/toggle", true,
        [](const std::string& p) {
            std::vector<std::string> all = {"on", "off", "toggle"};
            if (p.empty()) return all;
            std::vector<std::string> out;
            for (const auto& v : all)
                if (v.rfind(p, 0) == 0) out.push_back(v);
            return out;
        }});
    auto det_dup = std::make_unique<Command>();
    det_dup->name = "duplicate";
    det_dup->description = "Duplicate call detection";
    det_dup->args_usage = "<on|off>";
    det_dup->args.push_back({"value", "on/off", true,
        [](const std::string& p) {
            std::vector<std::string> all = {"on", "off"};
            if (p.empty()) return all;
            std::vector<std::string> out;
            for (const auto& v : all)
                if (v.rfind(p, 0) == 0) out.push_back(v);
            return out;
        }});
    det->add_subcommand(std::move(det_loop));
    det->add_subcommand(std::move(det_dup));
    set->add_subcommand(std::move(det));

    // /get
    auto get = std::make_unique<Command>();
    get->name = "get";
    get->description = "Show current settings";
    get->args_usage = "<setting>";
    auto gdet = std::make_unique<Command>();
    gdet->name = "detection";
    gdet->description = "Show detection settings";
    auto gdet_loop = std::make_unique<Command>();
    gdet_loop->name = "loop";
    gdet_loop->description = "Show loop detection state";
    auto gdet_dup = std::make_unique<Command>();
    gdet_dup->name = "duplicate";
    gdet_dup->description = "Show duplicate detection state";
    gdet->add_subcommand(std::move(gdet_loop));
    gdet->add_subcommand(std::move(gdet_dup));
    get->add_subcommand(std::move(gdet));
    cmds.push_back(std::move(get));

    // /model
    auto model = std::make_unique<Command>();
    model->name = "model";
    model->aliases = {"settings"};
    model->description = "Change LLM model";
    model->args_usage = "<model-name>";
    cmds.push_back(std::move(model));

    // /stop
    auto stop = std::make_unique<Command>();
    stop->name = "stop";
    stop->description = "Stop the running agent";
    cmds.push_back(std::move(stop));

    // /save
    auto save = std::make_unique<Command>();
    save->name = "save";
    save->description = "Save the current session";
    cmds.push_back(std::move(save));

    // /compress
    auto compress = std::make_unique<Command>();
    compress->name = "compress";
    compress->description = "Compress conversation history";
    cmds.push_back(std::move(compress));

    // Set must be last to test ordering
    cmds.push_back(std::move(set));

    return cmds;
}

// ---------------------------------------------------------------------------
// Token / has_arg / wants_open
// ---------------------------------------------------------------------------

TEST(token_empty) { ASSERT_EQ(token(""), ""); }
TEST(token_no_slash) { ASSERT_EQ(token("hello"), ""); }
TEST(token_just_slash) { ASSERT_EQ(token("/"), ""); }
TEST(token_name_only) { ASSERT_EQ(token("/set"), "set"); }
TEST(token_name_with_space) { ASSERT_EQ(token("/set detection"), "set"); }
TEST(token_name_trailing_space) { ASSERT_EQ(token("/set "), "set"); }

TEST(has_arg_no_space) { ASSERT(!has_arg("/set")); }
TEST(has_arg_with_space) { ASSERT(has_arg("/set detection")); }
TEST(has_arg_trailing_space) { ASSERT(!has_arg("")); }

TEST(wants_open_slash) { ASSERT(wants_open("/set")); }
TEST(wants_open_no_slash) { ASSERT(!wants_open("hello")); }
TEST(wants_open_empty) { ASSERT(!wants_open("")); }

// ---------------------------------------------------------------------------
// parse_input
// ---------------------------------------------------------------------------

TEST(parse_input_empty) {
    auto r = parse_input("");
    ASSERT(r.tokens.empty());
    ASSERT(r.partial.empty());
}

TEST(parse_input_just_slash) {
    auto r = parse_input("/");
    ASSERT(r.tokens.empty());
    ASSERT(r.partial.empty());
}

TEST(parse_input_name_only) {
    auto r = parse_input("/set");
    ASSERT(r.tokens.empty());
    ASSERT_EQ(r.partial, "set");
}

TEST(parse_input_name_with_trailing_space) {
    auto r = parse_input("/set ");
    ASSERT_EQ(r.tokens.size(), 1u);
    ASSERT_EQ(r.tokens[0], "set");
    ASSERT(r.ends_with_space);
    ASSERT(r.partial.empty());
}

TEST(parse_input_two_tokens) {
    auto r = parse_input("/set detection");
    ASSERT_EQ(r.tokens.size(), 1u);
    ASSERT_EQ(r.tokens[0], "set");
    ASSERT_EQ(r.partial, "detection");
}

TEST(parse_input_two_tokens_trailing_space) {
    auto r = parse_input("/set detection ");
    ASSERT_EQ(r.tokens.size(), 2u);
    ASSERT_EQ(r.tokens[0], "set");
    ASSERT_EQ(r.tokens[1], "detection");
    ASSERT(r.ends_with_space);
}

TEST(parse_input_three_tokens) {
    auto r = parse_input("/set detection loop");
    ASSERT_EQ(r.tokens.size(), 2u);
    ASSERT_EQ(r.tokens[0], "set");
    ASSERT_EQ(r.tokens[1], "detection");
    ASSERT_EQ(r.partial, "loop");
}

// ---------------------------------------------------------------------------
// common_prefix
// ---------------------------------------------------------------------------

TEST(common_prefix_empty) { ASSERT_EQ(common_prefix({}), ""); }
TEST(common_prefix_single) { ASSERT_EQ(common_prefix({"hello"}), "hello"); }
TEST(common_prefix_two) { ASSERT_EQ(common_prefix({"hello", "help"}), "hel"); }
TEST(common_prefix_no_match) { ASSERT_EQ(common_prefix({"abc", "xyz"}), ""); }
TEST(common_prefix_one_empty) { ASSERT_EQ(common_prefix({"abc", ""}), ""); }

// ---------------------------------------------------------------------------
// filter_top
// ---------------------------------------------------------------------------

TEST(filter_top_empty_token) {
    auto cmds = make_commands();
    auto m = filter_top(cmds, "");
    ASSERT_EQ(m.size(), cmds.size());
}

TEST(filter_top_prefix_match) {
    auto cmds = make_commands();
    auto m = filter_top(cmds, "s");
    // Matches: set (primary), save (primary), stop (primary), model (alias settings)
    ASSERT(m.size() >= 3);
}

TEST(filter_top_exact_match) {
    auto cmds = make_commands();
    auto m = filter_top(cmds, "stop");
    ASSERT_EQ(m.size(), 1u);
    ASSERT_EQ(m[0]->name, "stop");
}

TEST(filter_top_no_match) {
    auto cmds = make_commands();
    auto m = filter_top(cmds, "zzz");
    ASSERT(m.empty());
}

TEST(filter_top_alias_match) {
    auto cmds = make_commands();
    // "settings" is an alias for "model"
    auto m = filter_top(cmds, "settings");
    ASSERT_EQ(m.size(), 1u);
    ASSERT_EQ(m[0]->name, "model");
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

TEST(command_find_subcommand_exists) {
    auto cmds = make_commands();
    auto* set = cmds.back().get();
    ASSERT_EQ(set->name, "set");
    auto* det = set->find_subcommand("detection");
    ASSERT(det != nullptr);
    ASSERT_EQ(det->name, "detection");
    auto* loop = det->find_subcommand("loop");
    ASSERT(loop != nullptr);
    ASSERT_EQ(loop->name, "loop");
}

TEST(command_find_subcommand_missing) {
    auto cmds = make_commands();
    auto* set = cmds.back().get();
    ASSERT(set->find_subcommand("nosuch") == nullptr);
}

TEST(command_flatten) {
    auto cmds = make_commands();
    auto* set = cmds.back().get();
    auto flat = set->flatten();
    // set + detection + loop + duplicate = 4
    ASSERT_EQ(flat.size(), 4u);
}

TEST(command_usage_no_args) {
    auto cmds = make_commands();
    auto* stop = cmds[2].get(); // stop (index 2 in fixture: get(0) model(1) stop(2))
    ASSERT_EQ(stop->usage(), "/stop");
}

TEST(command_usage_with_args) {
    auto cmds = make_commands();
    auto* set = cmds.back().get();
    ASSERT_EQ(set->usage(), "/set <setting> <value>");
}

// ---------------------------------------------------------------------------
// Completer — top-level command completion
// ---------------------------------------------------------------------------

TEST(completer_empty_ambiguous) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/");
    // "/" is ambiguous (no common prefix among all names)
    ASSERT_EQ(r.new_input, "/");
    ASSERT(r.shadow.empty());
}

TEST(completer_partial_extends_to_prefix) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/co");
    ASSERT_EQ(r.new_input, "/compress");
    ASSERT(!r.shadow.empty());
    comp.reset();
}

TEST(completer_exact_match_adds_space) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/compress");
    ASSERT_EQ(r.new_input, "/compress ");
    ASSERT(r.close_drawer);
}

TEST(completer_no_match_unchanged) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/zzz");
    ASSERT_EQ(r.new_input, "/zzz");
}

TEST(completer_single_match_completes) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/sa");
    // Only "save" matches. The input extends to "/save" with shadow "ve"
    // shown inline. No trailing space yet (shadow indicates the rest).
    ASSERT_EQ(r.new_input, "/save");
    ASSERT(!r.shadow.empty());
}

// ---------------------------------------------------------------------------
// Completer — argument/subcommand completion
// ---------------------------------------------------------------------------

TEST(completer_arg_single_choice_completes) {
    auto cmds = make_commands();
    Completer comp;
    // /set detection l → extends to "loop" (subcommand match)
    auto r = comp.complete(cmds, "/set detection l");
    ASSERT_EQ(r.new_input, "/set detection loop ");
}

TEST(completer_arg_extends_to_common_prefix) {
    auto cmds = make_commands();
    Completer comp;
    // /set d → "detection" (extends; no trailing space because detection has
    // subcommands, so the user continues typing subcommand name)
    auto r = comp.complete(cmds, "/set d");
    ASSERT_EQ(r.new_input, "/set detection");
    ASSERT(r.close_drawer);
}

TEST(completer_arg_subcommand_completion) {
    auto cmds = make_commands();
    Completer comp;
    // /set det → extends to "detection" (no trailing space, detection has
    // subcommands so user continues typing)
    auto r = comp.complete(cmds, "/set det");
    ASSERT_EQ(r.new_input, "/set detection");
}

TEST(completer_arg_unknown_command_falls_back) {
    auto cmds = make_commands();
    Completer comp;
    // /nosuch arg → unknown command, falls to top-level
    auto r = comp.complete(cmds, "/nosuch arg");
    // Should fall back gracefully
    ASSERT(!r.new_input.empty());
}

TEST(completer_arg_no_choices_unchanged) {
    auto cmds = make_commands();
    Completer comp;
    // /set xyz → no match in subcommands or args
    auto r = comp.complete(cmds, "/set xyz");
    // Should not crash, may extend if partial matches anything at top level
    ASSERT_FALSE(r.new_input.empty());
}

// ---------------------------------------------------------------------------
// Multi-tab state machine
// ---------------------------------------------------------------------------

TEST(completer_first_tab_arms_second_tab_popup) {
    auto cmds = make_commands();
    Completer comp;
    // First Tab at "detection " (ambiguous between loop/duplicate)
    auto r1 = comp.complete(cmds, "/set detection ");
    // First Tab should arm, not popup
    ASSERT_FALSE(r1.show_popup);
    // Second consecutive Tab: show popup
    auto r2 = comp.complete(cmds, "/set detection ");
    ASSERT(r2.show_popup);
    ASSERT(r2.popup_items.size() >= 2u);
}

TEST(completer_second_tab_after_reset_arms_not_popup) {
    auto cmds = make_commands();
    Completer comp;
    comp.complete(cmds, "/set detection ");
    comp.reset();
    auto r = comp.complete(cmds, "/set detection ");
    ASSERT_FALSE(r.show_popup);
}

TEST(completer_different_input_resets_state) {
    auto cmds = make_commands();
    Completer comp;
    comp.complete(cmds, "/set detection ");
    // Different input → new sequence
    auto r = comp.complete(cmds, "/set detection l");
    ASSERT_EQ(r.new_input, "/set detection loop ");
    ASSERT_FALSE(r.show_popup);
}

// ---------------------------------------------------------------------------
// Cisco ?-style context-sensitive help
// ---------------------------------------------------------------------------

TEST(completer_question_top_level_shows_all) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/", true);
    ASSERT(r.help_lines.size() >= 3);
    // Should show descriptions
    bool found_set = false;
    for (const auto& line : r.help_lines) {
        if (line.find("set") != std::string::npos) found_set = true;
    }
    ASSERT(found_set);
}

TEST(completer_question_partial_filters) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/s", true);
    ASSERT(r.help_lines.size() >= 3);
    bool found = false;
    for (const auto& line : r.help_lines) {
        if (line.find("save") != std::string::npos) found = true;
    }
    ASSERT(found);
}

TEST(completer_question_arg_shows_subcommands) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/set ", true);
    ASSERT(r.help_lines.size() >= 1);
    bool found_detection = false;
    for (const auto& line : r.help_lines) {
        if (line.find("detection") != std::string::npos) found_detection = true;
    }
    ASSERT(found_detection);
}

// ---------------------------------------------------------------------------
// Shadow/inline completion
// ---------------------------------------------------------------------------

TEST(completer_shadow_on_partial_match) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/co");
    ASSERT(!r.shadow.empty());
    // Shadow should contain the remaining part of "compress"
    ASSERT_EQ(r.new_input, "/compress");
}

TEST(completer_no_shadow_on_exact_match) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/compress");
    // Exact match → space, no shadow
    ASSERT_EQ(r.new_input, "/compress ");
    ASSERT(r.shadow.empty());
}

TEST(completer_no_shadow_on_ambiguous) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/");
    ASSERT(r.shadow.empty());
}

// ---------------------------------------------------------------------------
// drawer_matches
// ---------------------------------------------------------------------------

TEST(drawer_matches_empty) {
    auto cmds = make_commands();
    Completer comp;
    auto m = comp.drawer_matches(cmds, "/");
    ASSERT_EQ(m.size(), cmds.size());
}

TEST(drawer_matches_partial) {
    auto cmds = make_commands();
    Completer comp;
    auto m = comp.drawer_matches(cmds, "/co");
    ASSERT_EQ(m.size(), 1u);
    ASSERT_EQ(m[0]->name, "compress");
}

TEST(drawer_matches_no_match) {
    auto cmds = make_commands();
    Completer comp;
    auto m = comp.drawer_matches(cmds, "/zzz");
    ASSERT(m.empty());
}

// ---------------------------------------------------------------------------
// Command aliases
// ---------------------------------------------------------------------------

TEST(command_alias_model_via_settings) {
    auto cmds = make_commands();
    // "settings" is an alias for "model"
    Completer comp;
    auto r = comp.complete(cmds, "/sett");
    // Extends to "/model" with shadow "el" (no trailing space on prefix ext)
    ASSERT_EQ(r.new_input, "/model");
    ASSERT(!r.shadow.empty());
}

TEST(command_alias_save_via_s) {
    auto cmds = make_commands();
    Completer comp;
    // "s" matches set, stop, save (primary) + model (alias)
    auto m = comp.drawer_matches(cmds, "/s");
    ASSERT(m.size() >= 3);
    // First match must be a primary match, not model (alias)
    ASSERT(m[0]->name != "model");
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(completer_just_slash_no_crash) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "/");
    ASSERT_EQ(r.new_input, "/");
}

TEST(completer_empty_input_no_crash) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "");
    ASSERT_EQ(r.new_input, "");
}

TEST(completer_no_slash_no_crash) {
    auto cmds = make_commands();
    Completer comp;
    auto r = comp.complete(cmds, "hello");
    ASSERT_EQ(r.new_input, "hello");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    return run_all();
}
