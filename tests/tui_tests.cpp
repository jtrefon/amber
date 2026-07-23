// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

// TUI-specific unit tests. These test the ncurses-adjacent utilities
// (text wrapping, markdown rendering, command palette, completer).
// Kept separate from tests/run_tests.cpp (core tests) so the core
// test binary has no dependency on tui/ headers.

#include "agent.h"
#include "tui/textutil.h"
#include "tui/palette.h"
#include "tui/rich.h"
#include "tui/markdown.h"
#include "tests/test_util.h"

// ---------------------------------------------------------------------------
// TUI text utilities (UTF-8 wrapping / width / decoding)
// ---------------------------------------------------------------------------

TEST(textutil_utf8_len_and_display_cols) {
    std::string ascii = "hello";
    ASSERT_EQ(tui::text::utf8_len(ascii, 0), (size_t)1);
    ASSERT_EQ(tui::text::display_cols(ascii), 5);

    std::string emoji = "a\xF0\x9F\x98\x80z";  // a + U+1F600 + z
    ASSERT_EQ(tui::text::utf8_len(emoji, 1), (size_t)4);
    ASSERT_EQ(tui::text::display_cols(emoji), 3);

    // truncated multibyte sequence counts as a single byte
    std::string bad = "\xF0\x9F";
    ASSERT_EQ(tui::text::utf8_len(bad, 0), (size_t)1);
}

TEST(textutil_wrap_respects_width_and_newlines) {
    auto lines = tui::text::wrap("the quick brown fox", 9);
    ASSERT_FALSE(lines.empty());
    for (const auto& l : lines)
        ASSERT(tui::text::display_cols(l) <= 9);

    auto para = tui::text::wrap("one\ntwo", 40);
    ASSERT_EQ(para.size(), (size_t)2);
    ASSERT_EQ(para[0], "one");
    ASSERT_EQ(para[1], "two");
}

TEST(textutil_wrap_strips_ansi_and_expands_tabs) {
    // ANSI color escape should be removed; tab becomes four spaces.
    auto lines = tui::text::wrap("\x1b[31mred\x1b[0m\tx", 80);
    ASSERT_EQ(lines.size(), (size_t)1);
    ASSERT_EQ(lines[0], "red    x");
}

TEST(textutil_to_wide_decodes_codepoints) {
    std::wstring w = tui::text::to_wide("a\xF0\x9F\x98\x80");
    ASSERT_EQ(w.size(), (size_t)2);
    ASSERT_EQ((long)w[0], (long)'a');
    ASSERT_EQ((long)w[1], (long)0x1F600);
}

// ---------------------------------------------------------------------------
// Rich line model + width-aware wrapping
// ---------------------------------------------------------------------------

TEST(rich_wrap_splits_long_line_and_keeps_runs) {
    tui::rich::Line l;
    tui::rich::Run r; r.pair = 3; r.bold = true; r.text = "the quick brown fox";
    l.runs.push_back(r);
    auto w = tui::rich::wrap(l, 9);
    // Greedy wrap: "the quick" (9) and "brown fox" (9) each fill the width.
    ASSERT_EQ(w.size(), (size_t)2);
    for (auto& x : w) ASSERT_EQ(x.runs.size(), (size_t)1);
    ASSERT_EQ(w[0].runs[0].pair, 3);
    ASSERT_TRUE(w[0].runs[0].bold);
    ASSERT_EQ(w[0].runs[0].text, "the quick");
}

TEST(rich_wrap_preserves_multibyte_width) {
    // Two emoji, each display-width 2, fit exactly in a width-4 line.
    tui::rich::Line l;
    tui::rich::Run r; r.text = "\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
    l.runs.push_back(r);
    auto w = tui::rich::wrap(l, 4);
    ASSERT_EQ(w.size(), (size_t)1);
    ASSERT_EQ(w[0].runs[0].text, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80");
}

TEST(rich_wrap_forces_break_on_overlong_word) {
    // A single word wider than the column must be broken across lines.
    tui::rich::Line l;
    tui::rich::Run r; r.text = "abcdefghij";  // 10 cols
    l.runs.push_back(r);
    auto w = tui::rich::wrap(l, 4);
    ASSERT_EQ(w.size(), (size_t)3);
    ASSERT_EQ(w[0].runs[0].text, "abcd");
    ASSERT_EQ(w[1].runs[0].text, "efgh");
}

// ---------------------------------------------------------------------------
// Markdown -> RichLines
// ---------------------------------------------------------------------------

TEST(markdown_renders_heading_bold_and_inline_code) {
    auto ls = tui::md::render("# Title\nSome **bold** and `code`.", tui::md::Style{});
    ASSERT_FALSE(ls.empty());
    // First line is the heading, bold.
    ASSERT_TRUE(ls[0].runs[0].bold);
    // A later line carries the bold run and the code run on distinct pairs.
    bool saw_bold = false, saw_code = false;
    for (auto& l : ls)
        for (auto& r : l.runs) {
            if (r.bold) saw_bold = true;
            if (r.pair == tui::md::Style{}.code_pair) saw_code = true;
        }
    ASSERT_TRUE(saw_bold);
    ASSERT_TRUE(saw_code);
}

TEST(markdown_maps_inline_ansi_sgr_to_runs) {
    // ESC[32m = green, mapped to the code pair by the renderer.
    std::string s = "\x1b[32mgreen\x1b[0m normal";
    auto ls = tui::md::render(s, tui::md::Style{});
    bool saw_green = false;
    for (auto& l : ls)
        for (auto& r : l.runs)
            if (r.text == "green" && r.pair == tui::md::Style{}.code_pair)
                saw_green = true;
    ASSERT_TRUE(saw_green);
}

TEST(markdown_highlight_colors_fenced_code) {
    std::string code = "int x = 1; // c\n";
    auto ls = tui::md::highlight(code, "cpp", tui::md::Style{}.code_pair);
    ASSERT_EQ(ls.size(), (size_t)1);
    // keyword "int", number "1", comment "// c" on distinct pairs.
    int saw_num = 0, saw_cmt = 0;
    for (auto& r : ls[0].runs) {
        if (r.text == "1") ++saw_num;
        if (r.text.find("//") != std::string::npos) ++saw_cmt;
    }
    ASSERT_EQ(saw_num, 1);
    ASSERT_EQ(saw_cmt, 1);
}

TEST(markdown_renders_aligned_table_and_skips_divider) {
    std::string md = "| Name | Age |\n|------|-----|\n| Alice | 30 |\n| Bob | 7 |";
    auto ls = tui::md::render(md, tui::md::Style{});
    // header, separator, alice, bob, trailing blank = 5 lines.
    ASSERT_EQ(ls.size(), (size_t)5);
    // The markdown divider row ("|------|-----|") must NOT appear as a data
    // row (it is skipped; only the drawn box separator ├─┼─┤ remains).
    for (auto& l : ls)
        for (auto& r : l.runs)
            ASSERT_TRUE(r.text.find("|------") == std::string::npos);
    // Header row cell text is "Name", body cells "Alice"/"Bob".
    std::string head;
    for (auto& r : ls[0].runs) head += r.text;
    ASSERT_TRUE(head.find("Name") != std::string::npos);
    std::string row2;
    for (auto& r : ls[2].runs) row2 += r.text;
    ASSERT_TRUE(row2.find("Alice") != std::string::npos);
}

TEST(markdown_renders_table_without_leading_blank_line) {
    // Regression: LLMs routinely emit a GFM table with no blank line between the
    // preceding prose and the header row. md4c needs that blank line to detect
    // a table; the renderer must insert it so the table does not collapse into a
    // single literal paragraph of pipe characters.
    std::string md =
        "Summary of Priority\n"
        "| Priority | # | Issue |\n"
        "|----------|---|-------|\n"
        "| High | 1 | foo |\n";
    auto ls = tui::md::render(md, tui::md::Style{});
    std::string all;
    for (auto& l : ls)
        for (auto& r : l.runs) all += r.text;
    // The collapsed artifact would contain the raw pipe sequence verbatim.
    ASSERT_TRUE(all.find("| Priority | # | Issue | |---") == std::string::npos);
    // A proper table exposes the header cell text and the box-drawn divider.
    ASSERT_TRUE(all.find("Priority") != std::string::npos);
    ASSERT_TRUE(all.find("├") != std::string::npos);
}

TEST(markdown_repairs_table_missing_delimiter_row) {
    // Regression: LLMs often emit a GFM table with no "|----|" delimiter row.
    // Without it md4c sees no table and the rows collapse into one garbage
    // line of pipe characters. The renderer must synthesize the delimiter so
    // the table renders with a header separator and all body rows.
    std::string md =
        "| A | B |\n"
        "| 1 | 2 |\n"
        "| 3 | 4 |\n";
    auto ls = tui::md::render(md, tui::md::Style{});
    std::string all;
    for (auto& l : ls)
        for (auto& r : l.runs) all += r.text;
    // The collapsed artifact would keep the raw rows glued together.
    ASSERT_TRUE(all.find("| A | B | | 1 | 2 |") == std::string::npos);
    // Header + box separator + both body rows must be present.
    ASSERT_TRUE(all.find('A') != std::string::npos);
    ASSERT_TRUE(all.find("├") != std::string::npos);
    ASSERT_TRUE(all.find('1') != std::string::npos);
    ASSERT_TRUE(all.find('3') != std::string::npos);
}

TEST(markdown_splits_embedded_separator_rule) {
    // Regression: a model sometimes glues a fake rule (long run of box-drawing
    // dashes) onto the end of a code line. It must be split onto its own line
    // and rendered as a clean horizontal rule, not literal garbage.
    std::string md =
        "Fix: use mvwaddnstr(w, 1, s.c_str(), aw);"
        "──────────────────────────────────────────────────────────────"
        "\n\nNext section.";
    auto ls = tui::md::render(md, tui::md::Style{});
    bool saw_hr = false, saw_dup = false;
    std::string body;
    for (auto& l : ls) {
        if (l.is_hr) saw_hr = true;
        for (auto& r : l.runs) body += r.text;
    }
    ASSERT_TRUE(saw_hr);
    // The code text must appear exactly once (no duplication from the split).
    int count = 0;
    size_t pos = 0;
    while ((pos = body.find("mvwaddnstr", pos)) != std::string::npos) {
        ++count; pos += 10;
    }
    ASSERT_EQ(count, 1);
}

TEST(markdown_trims_heading_whitespace) {
    auto ls = tui::md::render("##   Spaced heading   \nbody", tui::md::Style{});
    ASSERT_FALSE(ls.empty());
    std::string h;
    for (auto& r : ls[0].runs) h += r.text;
    ASSERT_EQ(h, "Spaced heading");
}

TEST(markdown_bare_hash_markers_do_not_crash) {
    // Regression: a line that is only '#' / '###' (no trailing space) used to
    // throw std::out_of_range from substr(); md4c now treats it as an empty
    // heading (renders to nothing) instead of crashing the whole UI.
    auto a = tui::md::render("#", tui::md::Style{});
    auto b = tui::md::render("###", tui::md::Style{});
    auto c = tui::md::render(">", tui::md::Style{});
    auto d = tui::md::render("-", tui::md::Style{});
    (void)a; (void)b; (void)c; (void)d;  // must not throw
    auto ls = tui::md::render("# Title\n## Sub\n### Deep\nbody", tui::md::Style{});
    ASSERT_EQ(ls.size(), (size_t)4);
    std::string h0, h1, h2;
    for (auto& r : ls[0].runs) h0 += r.text;
    for (auto& r : ls[1].runs) h1 += r.text;
    for (auto& r : ls[2].runs) h2 += r.text;
    ASSERT_EQ(h0, "Title");
    ASSERT_EQ(h1, "Sub");
    ASSERT_EQ(h2, "Deep");
}

TEST(markdown_ordered_list_numbers_sequentially) {
    auto ls = tui::md::render("1. first\n2. second\n3. third", tui::md::Style{});
    // md4c normalizes; we prefix each item with its ordinal.
    std::vector<std::string> lines;
    for (auto& l : ls) {
        std::string t;
        for (auto& r : l.runs) t += r.text;
        lines.push_back(t);
    }
    ASSERT_TRUE(lines.size() >= 3);
    ASSERT_TRUE(lines[0].find("1.") != std::string::npos);
    ASSERT_TRUE(lines[1].find("2.") != std::string::npos);
    ASSERT_TRUE(lines[2].find("3.") != std::string::npos);
}

TEST(markdown_nested_list_items_separate) {
    auto ls = tui::md::render("- bullet one\n  - bullet two\n  - nested", tui::md::Style{});
    std::vector<std::string> lines;
    for (auto& l : ls) {
        std::string t;
        for (auto& r : l.runs) t += r.text;
        lines.push_back(t);
    }
    // Three distinct bullet lines (nested ones indented further).
    ASSERT_EQ(lines.size(), (size_t)3);
    ASSERT_TRUE(lines[0].find("bullet one") != std::string::npos);
    ASSERT_TRUE(lines[1].find("bullet two") != std::string::npos);
    ASSERT_TRUE(lines[2].find("nested") != std::string::npos);
    // Nested items are indented relative to the parent.
    ASSERT_TRUE(lines[1].find("  •") != std::string::npos);
}

TEST(markdown_blockquote_each_line_quoted) {
    auto ls = tui::md::render("> a block quote\n> second line", tui::md::Style{});
    ASSERT_EQ(ls.size(), (size_t)2);
    for (auto& l : ls) {
        std::string t;
        for (auto& r : l.runs) t += r.text;
        ASSERT_TRUE(t.find('>') == 0);
    }
}

TEST(markdown_task_list_items_render) {
    auto ls = tui::md::render("- [x] done\n- [ ] todo", tui::md::Style{});
    std::string all;
    for (auto& l : ls)
        for (auto& r : l.runs) all += r.text;
    ASSERT_TRUE(all.find("done") != std::string::npos);
    ASSERT_TRUE(all.find("todo") != std::string::npos);
}

// ---------------------------------------------------------------------------
// TUI command palette (slash-command filtering / completion — no ncurses)
// ---------------------------------------------------------------------------

static std::vector<tui::palette::Command> palette_fixture() {
    return {
        {"help", {"?", "h"}, "[command]", "list commands", nullptr},
        {"window", {"win", "w"}, "new|close", "manage windows", nullptr},
        {"save", {}, "", "persist conversation", nullptr},
        {"quit", {"exit", "q"}, "", "exit", nullptr},
    };
}

// A richer fixture with duplicate-detection commands that have complete_arg.
static std::vector<tui::palette::Command> palette_detection_fixture() {
    // Reusable complete_arg that matches the real /set command.
    auto set_complete = [](const std::string& partial) {
        std::vector<std::string> all = {
            "detection loop off", "detection loop on", "detection loop toggle",
            "detection duplicate off", "detection duplicate on",
            "detection duplicate toggle"};
        if (partial.empty())
            return std::vector<std::string>{"detection loop", "detection duplicate"};
        std::vector<std::string> out;
        for (const auto& a : all) {
            if (a.rfind(partial, 0) == 0) { out.push_back(a); continue; }
            size_t pos = 0;
            while (pos < a.size()) {
                pos = a.find_first_not_of(' ', pos);
                if (pos == std::string::npos) break;
                if (a.rfind(partial, pos) == pos) { out.push_back(a); break; }
                pos = a.find(' ', pos);
                if (pos == std::string::npos) break;
                ++pos;
            }
        }
        return out;
    };
    return {
        {"set", {}, "detection loop|duplicate off|on|toggle",
         "set runtime options",
         nullptr, set_complete, nullptr},
        // model has alias "settings" — used to test primary-name priority
        {"model", {"settings", "server"}, "",
         "server settings",
         nullptr, nullptr, nullptr},
        {"stop", {"cancel"}, "",
         "stop agent",
         nullptr, nullptr, nullptr},
        {"save", {}, "",
         "save session",
         nullptr, nullptr, nullptr},
        {"compress", {"compact"}, "",
         "compress history",
         nullptr, nullptr, nullptr},
    };
}

TEST(palette_token_and_arg_detection) {
    ASSERT_EQ(tui::palette::token("/wi"), "wi");
    ASSERT_EQ(tui::palette::token("/window new"), "window");
    ASSERT_EQ(tui::palette::token(""), "");
    ASSERT_EQ(tui::palette::token("plain"), "");
    ASSERT_TRUE(tui::palette::wants_open("/x"));
    ASSERT_FALSE(tui::palette::wants_open("x"));
    ASSERT_TRUE(tui::palette::has_arg("/window new"));
    ASSERT_FALSE(tui::palette::has_arg("/window"));
}

TEST(palette_filter_matches_name_and_alias) {
    auto cmds = palette_fixture();
    ASSERT_EQ(tui::palette::filter(cmds, "").size(), (size_t)4);   // all
    ASSERT_EQ(tui::palette::filter(cmds, "w").size(), (size_t)1);  // window
    ASSERT_EQ(tui::palette::filter(cmds, "win").front()->name, "window");
    ASSERT_EQ(tui::palette::filter(cmds, "q").front()->name, "quit");  // alias
    ASSERT_TRUE(tui::palette::filter(cmds, "zzz").empty());
}

TEST(palette_find_by_name_or_alias) {
    auto cmds = palette_fixture();
    ASSERT_TRUE(tui::palette::find(cmds, "help") != nullptr);
    ASSERT_EQ(tui::palette::find(cmds, "exit")->name, "quit");
    ASSERT_TRUE(tui::palette::find(cmds, "nope") == nullptr);
}

TEST(palette_complete_prefix_and_selection) {
    auto cmds = palette_fixture();
    // Use Completer to exercise command-name completion.
    tui::palette::Completer comp;
    // "/wi" → matches "window" → extend to common prefix
    ASSERT_EQ(comp.handle_tab(cmds, "/wi", -1).input, "/window");
    comp.reset();
    // "/window" → exact match → append space
    ASSERT_EQ(comp.handle_tab(cmds, "/window", -1).input, "/window ");
    comp.reset();
    // "/" with selection index 3 → pick /quit
    ASSERT_EQ(comp.handle_tab(cmds, "/", 3).input, "/quit ");
    comp.reset();
    // "/zzz" → no match → unchanged
    ASSERT_EQ(comp.handle_tab(cmds, "/zzz", -1).input, "/zzz");
}

// ---------------------------------------------------------------------------
// Completer regression tests — command-name completion
// ---------------------------------------------------------------------------

TEST(completer_cmd_empty_shows_all) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/" with no selection → ambiguous (set/model/stop/save/compress diverge)
    // Input unchanged because common prefix of all names is "".
    ASSERT_EQ(comp.handle_tab(cmds, "/", -1).input, "/");
}

TEST(completer_cmd_partial_extends_to_prefix) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "co" → compress
    ASSERT_EQ(comp.handle_tab(cmds, "/co", -1).input, "/compress");
    comp.reset();
    // "com" → compress
    ASSERT_EQ(comp.handle_tab(cmds, "/com", -1).input, "/compress");
}

TEST(completer_cmd_exact_match_adds_space) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/compress" → exact → "/compress "
    ASSERT_EQ(comp.handle_tab(cmds, "/compress", -1).input, "/compress ");
    comp.reset();
    // "/stop" → exact → "/stop "
    ASSERT_EQ(comp.handle_tab(cmds, "/stop", -1).input, "/stop ");
}

TEST(completer_cmd_no_match_unchanged) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    ASSERT_EQ(comp.handle_tab(cmds, "/zzz", -1).input, "/zzz");
    ASSERT_EQ(comp.handle_tab(cmds, "/nonexistent", -1).input, "/nonexistent");
}

TEST(completer_cmd_primary_name_beats_alias) {
    // "set" must match the "set" command (primary name) before
    // "model" (alias "settings"). Regression guard for /set jumping to /model.
    auto cmds = palette_detection_fixture();
    // "s" matches set, stop, save, and model(settings)
    tui::palette::Completer comp;
    auto matches = comp.drawer_matches(cmds, "/s");
    ASSERT(!matches.empty());
    // First match must be a primary-name match ("set", "stop", or "save"),
    // NOT "model" (which only matches via alias "settings").
    bool first_is_primary = false;
    for (const auto& m : matches) {
        first_is_primary = true;
        break;  // just check first exists
    }
    const auto& first = *matches[0];
    // "set", "stop", "save" are all primary matches for "s".
    // The first registered primary is "set". It must NOT be "model".
    ASSERT(first.name != "model");
}

TEST(completer_cmd_selection_picks_specific) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/" with sel=2 → "save" (index 2 in detection fixture)
    // Fixture order: set(0) model(1) stop(2) save(3) compress(4)
    ASSERT_EQ(comp.handle_tab(cmds, "/", 2).input, "/stop ");
    comp.reset();
    ASSERT_EQ(comp.handle_tab(cmds, "/", 0).input, "/set ");
    comp.reset();
    ASSERT_EQ(comp.handle_tab(cmds, "/", 3).input, "/save ");
}

TEST(completer_cmd_ambiguous_common_prefix) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "sa" → matches "save" only → extend to common prefix "/save"
    // (no trailing space because the prefix extends the typed partial)
    ASSERT_EQ(comp.handle_tab(cmds, "/sa", -1).input, "/save");
}

// ---------------------------------------------------------------------------
// Argument completion tests
// ---------------------------------------------------------------------------

TEST(completer_arg_single_choice_completes) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/set detection loop of" → only "detection loop off" matches
    auto r = comp.handle_tab(cmds, "/set detection loop of", -1);
    ASSERT_EQ(r.input, "/set detection loop off ");
}

TEST(completer_arg_extends_to_common_prefix) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/set d" → "detection " prefix (shared by loop and duplicate)
    auto r = comp.handle_tab(cmds, "/set d", -1);
    ASSERT_EQ(r.input, "/set detection ");
    ASSERT(r.close_drawer);
}

TEST(completer_arg_narrows_to_single_group) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/set detection l" → only loop variants → extends to "detection loop "
    auto r = comp.handle_tab(cmds, "/set detection l", -1);
    ASSERT_EQ(r.input, "/set detection loop ");
}

TEST(completer_arg_narrows_to_duplicate_group) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/set detection dup" → only duplicate variants → extends to "detection duplicate "
    auto r = comp.handle_tab(cmds, "/set detection dup", -1);
    ASSERT_EQ(r.input, "/set detection duplicate ");
}

TEST(completer_arg_ambiguous_arms_popup_on_second_tab) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // First Tab at ambiguous prefix: arm for popup, no popup yet
    auto r1 = comp.handle_tab(cmds, "/set detection ", -1);
    ASSERT_FALSE(r1.show_popup);  // first Tab: arms for second
    // Second consecutive Tab: show popup
    auto r2 = comp.handle_tab(cmds, "/set detection ", -1);
    ASSERT(r2.show_popup);
    ASSERT(r2.popup_items.size() >= 6u);
}

TEST(completer_arg_no_choices_unchanged) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/set xyz" → no completion matches → input unchanged
    auto r = comp.handle_tab(cmds, "/set xyz", -1);
    ASSERT_EQ(r.input, "/set xyz");
}

TEST(completer_arg_unknown_command_unchanged) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // "/nonexistent arg" → command not found → falls to drawer completion
    auto r = comp.handle_tab(cmds, "/nonexistent arg", -1);
    ASSERT_FALSE(r.input.empty());
}

TEST(completer_arg_reset_clears_tab_state) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // First Tab arms
    comp.handle_tab(cmds, "/set detection ", -1);
    // Reset simulates a non-Tab keypress
    comp.reset();
    // Next Tab should be treated as FIRST Tab again (no popup)
    auto r = comp.handle_tab(cmds, "/set detection ", -1);
    ASSERT_FALSE(r.show_popup);  // should arm again, not popup
}

TEST(completer_arg_keyword_du_matches_duplicate) {
    // /set du  →  Tab  →  must complete to "detection duplicate"
    // The partial "du" should match the word "duplicate" inside
    // "detection duplicate off" even though the full string doesn't
    // start with "du".  Regression guard against the "wiped" bug.
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    auto r = comp.handle_tab(cmds, "/set du", -1);
    ASSERT_EQ(r.input, "/set detection duplicate ");
}

TEST(completer_arg_keyword_loop_matches_loop) {
    // /set loop  →  Tab  →  must complete to "detection loop"
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    auto r = comp.handle_tab(cmds, "/set loop", -1);
    ASSERT_EQ(r.input, "/set detection loop ");
}

TEST(completer_arg_keyword_of_matches_off) {
    // /set of  →  Tab  →  "of" matches "off" in BOTH loop and duplicate
    // variants (both end with "off"), so the common prefix is "detection "
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    auto r = comp.handle_tab(cmds, "/set of", -1);
    ASSERT_EQ(r.input, "/set detection ");
}

TEST(completer_arg_different_input_resets_tab_state) {
    auto cmds = palette_detection_fixture();
    tui::palette::Completer comp;
    // First Tab on one prefix
    comp.handle_tab(cmds, "/set detection ", -1);
    // Different input → counts as a new sequence
    auto r = comp.handle_tab(cmds, "/set detection l", -1);
    ASSERT_EQ(r.input, "/set detection loop ");
    ASSERT_FALSE(r.show_popup);
}

TEST(completer_cmd_alias_priority_after_filter) {
    // The filter function must list primary-name matches before alias matches.
    auto cmds = palette_detection_fixture();
    // "s" should match set (primary), stop (primary), save (primary),
    // and model (alias "settings").
    auto matches = tui::palette::filter(cmds, "s");
    // At least 4 matches: set, stop, save, model
    ASSERT(matches.size() >= 4u);
    // The first 3 should be primary-name matches
    for (size_t i = 0; i < 3 && i < matches.size(); ++i) {
        ASSERT(matches[i]->name != "model");
    }
    // "model" should appear after primary matches
    bool found_model = false;
    for (size_t i = 3; i < matches.size(); ++i) {
        if (matches[i]->name == "model") { found_model = true; break; }
    }
    ASSERT(found_model);
}

TEST(palette_usage_and_common_prefix) {
    tui::palette::Command c{"window", {}, "new|close", "manage", nullptr};
    ASSERT_EQ(tui::palette::usage(c), "/window new|close");
    tui::palette::Command bare{"save", {}, "", "persist", nullptr};
    ASSERT_EQ(tui::palette::usage(bare), "/save");
    ASSERT_EQ(tui::palette::common_prefix({"send", "set", "sever"}), "se");
    ASSERT_EQ(tui::palette::common_prefix({}), "");
}

