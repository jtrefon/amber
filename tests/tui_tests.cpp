
// TUI-specific unit tests. These test the ncurses-adjacent utilities
// (text wrapping, markdown rendering, command palette, completer).
// Kept separate from tests/run_tests.cpp (core tests) so the core
// test binary has no dependency on tui/ headers.

#include "agent.h"
#include "tui/action_registry.h"
#include "tui/textutil.h"
#include "tui/palette.h"
#include "tui/rich.h"
#include "tui/markdown.h"
#include "tui/tool_display.h"
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
    bool saw_hr = false;
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
        {"help", "core.help", {"?", "h"}, "[command]", "list commands",},
        {"window", "core.window", {"win", "w"}, "new|close", "manage windows",},
        {"save", "core.session.save", {}, "", "persist conversation",},
        {"quit", "core.quit", {"exit", "q"}, "", "exit",},
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


TEST(palette_usage_and_common_prefix) {
    tui::palette::Command c{"window", "core.window", {}, "new|close", "manage",};
    ASSERT_EQ(tui::palette::usage(c), "/window new|close");
    tui::palette::Command bare{"save", "core.session.save", {}, "", "persist",};
    ASSERT_EQ(tui::palette::usage(bare), "/save");
    ASSERT_EQ(tui::palette::common_prefix({"send", "set", "sever"}), "se");
    ASSERT_EQ(tui::palette::common_prefix({}), "");
}


TEST(git_prompt_no_diff) {
    std::string r = tui::text::git_prompt("myproject", "main", 0, 0);
    ASSERT(r.find("myproject") != std::string::npos);
    ASSERT(r.find("main") != std::string::npos);
    ASSERT(!r.empty());
    // First char should be the box-drawing ┌ (U+250C, 3 UTF-8 bytes: E2 94 8C)
    ASSERT_EQ(static_cast<unsigned char>(r[0]), 0xE2);
    ASSERT_EQ(static_cast<unsigned char>(r[1]), 0x94);
    ASSERT_EQ(static_cast<unsigned char>(r[2]), 0x8C);
    // Should contain the ❯ delimiter
    ASSERT(r.find("\u276f") != std::string::npos);
    // No +/- indicators
    ASSERT(r.find('+') == std::string::npos);
    ASSERT(r.find('-') == std::string::npos);
}


TEST(git_prompt_with_diff) {
    std::string r = tui::text::git_prompt("proj", "feature/x", 3, 1);
    ASSERT(r.find("proj") != std::string::npos);
    ASSERT(r.find("feature/x") != std::string::npos);
    ASSERT(r.find("+3") != std::string::npos);
    ASSERT(r.find("-1") != std::string::npos);
}


TEST(git_prompt_insertions_only) {
    std::string r = tui::text::git_prompt("x", "fix", 5, 0);
    ASSERT(r.find("+5") != std::string::npos);
}


TEST(git_prompt_deletions_only) {
    std::string r = tui::text::git_prompt("x", "fix", 0, 2);
    ASSERT(r.find("-2") != std::string::npos);
}


TEST(git_prompt_empty_project) {
    std::string r = tui::text::git_prompt("", "main", 0, 0);
    ASSERT(r.find("main") != std::string::npos);
}


TEST(col_to_byte_ascii) {
    // "hello" = 5 bytes, 5 columns
    ASSERT_EQ(tui::text::col_to_byte("hello", 0), (size_t)0);
    ASSERT_EQ(tui::text::col_to_byte("hello", 3), (size_t)3);
    ASSERT_EQ(tui::text::col_to_byte("hello", 5), (size_t)5);
    ASSERT_EQ(tui::text::col_to_byte("hello", 99), (size_t)5);
}


TEST(col_to_byte_utf8) {
    // "┌ a" = ┌(3 bytes,1 col) + space(1 byte,1 col) + a(1 byte,1 col)
    std::string s = "\u250c a";
    ASSERT_EQ(tui::text::col_to_byte(s, 0), (size_t)0);   // col 0 → byte 0
    ASSERT_EQ(tui::text::col_to_byte(s, 1), (size_t)3);   // col 1 → byte 3 (past ┌)
    ASSERT_EQ(tui::text::col_to_byte(s, 2), (size_t)4);   // col 2 → byte 4 (past space)
    ASSERT_EQ(tui::text::col_to_byte(s, 3), (size_t)5);   // col 3 → byte 5 (past 'a')
}


TEST(col_to_byte_git_prompt) {
    std::string p = tui::text::git_prompt("proj", "fix", 3, 1);
    // p = "┌ proj fix +3/-1 ❯ " (roughly: ┌=1col, sp=1, proj=4, sp=1, fix=3, ...)
    // Verify column 0 always maps to byte 0.
    ASSERT_EQ(tui::text::col_to_byte(p, 0), (size_t)0);
    // Verify the whole string is reachable.
    int total = tui::text::display_cols(p);
    ASSERT(tui::text::col_to_byte(p, total) <= p.size());
}


TEST(textutil_spinner_frames) {
    // The round spinner cycle; the ASCII fallback must also yield a full
    // frame cycle so animation never stalls.
    for (int i = 0; i < 4; ++i)
        ASSERT(!std::string(tui::text::glyph::spinner_round(i)).empty());
    ASSERT(std::string(tui::text::glyph::spinner_round(0)) !=
           std::string(tui::text::glyph::spinner_round(1)));
}

// ── Test: action registry is idempotent (feeds re-merge on refresh) ──

TEST(action_registry_idempotent_register) {
    tui::ActionRegistry reg;
    std::vector<std::string> calls;
    reg.register_action("core.test.a", [&](const std::string&) {
        calls.emplace_back("first");
    });
    // Re-registering the same key (a feed refresh while the handler runs)
    // must NOT replace the live lambda.
    reg.register_action("core.test.a", [&](const std::string&) {
        calls.emplace_back("second");
    });
    ASSERT(reg.has("core.test.a"));
    ASSERT(reg.dispatch("core.test.a", "x"));
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0], "first");
    ASSERT_FALSE(reg.dispatch("core.test.missing", ""));
}

// ---------------------------------------------------------------------------
// Tool display: human-readable call description + single-line close
// (feat/tool-display-revamp)
// ---------------------------------------------------------------------------

namespace {

tui::rich::Line ts_line(const std::string& body) {
    tui::rich::Line ln;
    ln.runs.push_back({"[10:00:00] ", tui::P_REASONING, false, true});
    tui::rich::Run r;
    r.pair = tui::P_STATUS;
    r.text = body;
    ln.runs.push_back(std::move(r));
    return ln;
}

} // namespace

TEST(tool_display_bash_shows_command_not_name) {
    std::string d = tui::tool_display::describe_tool_call(
        "bash", agent::json{{"command", "grep -rn CancellationToken src/ include/"}});
    ASSERT(d.find("bash") == std::string::npos);
    ASSERT(d.find("grep -rn CancellationToken src/ include/") !=
           std::string::npos);
}

TEST(tool_display_bash_long_command_truncated) {
    std::string long_cmd(300, 'x');
    std::string d = tui::tool_display::describe_tool_call(
        "bash", agent::json{{"command", long_cmd}});
    ASSERT(d.size() < long_cmd.size());
    ASSERT(d.find("…") != std::string::npos);
}

TEST(tool_display_read_shows_path) {
    std::string d = tui::tool_display::describe_tool_call(
        "read", agent::json{{"path", "include/agent/config.h"}});
    ASSERT_EQ(d, "read include/agent/config.h");
}

TEST(tool_display_write_shows_path) {
    std::string d = tui::tool_display::describe_tool_call(
        "write", agent::json{{"path", "lib/compressor.cpp"},
                             {"edits", agent::json::array()}});
    ASSERT_EQ(d, "write lib/compressor.cpp");
}

TEST(tool_display_search_shows_pattern_and_path) {
    std::string d = tui::tool_display::describe_tool_call(
        "search", agent::json{{"pattern", "CancellationToken"},
                              {"path", "src/"}});
    ASSERT_EQ(d, "search CancellationToken in src/");
    std::string d2 = tui::tool_display::describe_tool_call(
        "search", agent::json{{"pattern", "foo"}});
    ASSERT_EQ(d2, "search foo");
}

TEST(tool_display_unknown_tool_falls_back) {
    std::string d = tui::tool_display::describe_tool_call(
        "todowrite", agent::json{{"task", "x"}});
    ASSERT(d.find("todowrite") == 0);
    ASSERT(!d.empty());
}

TEST(tool_display_empty_args_safe) {
    ASSERT(!tui::tool_display::describe_tool_call("bash", agent::json::object())
                .empty());
    ASSERT(!tui::tool_display::describe_tool_call("bash", agent::json::array())
                .empty());
}

TEST(tool_display_close_keeps_timestamp) {
    tui::rich::Line open = ts_line("◐ ls -la");
    tui::rich::Line summary;
    tui::rich::Run icon;
    icon.pair = tui::P_GIT_PLUS;
    icon.text = "✓";
    tui::rich::Run rest;
    rest.pair = tui::P_STATUS;
    rest.text = " ls -la → exit 0 (3 lines)";
    summary.runs.push_back(std::move(icon));
    summary.runs.push_back(std::move(rest));
    tui::rich::Line closed = tui::tool_display::close_tool_line(open, summary);
    ASSERT(!closed.runs.empty());
    ASSERT_EQ(closed.runs[0].text, "[10:00:00] ");
    ASSERT(closed.runs[0].dim);
    ASSERT(closed.runs.size() >= 3u);
}

TEST(tool_display_elapsed_label_formatting) {
    ASSERT_EQ(tui::tool_display::elapsed_label(0), "0s");
    ASSERT_EQ(tui::tool_display::elapsed_label(12), "12s");
    ASSERT_EQ(tui::tool_display::elapsed_label(65), "1m 05s");
    ASSERT_EQ(tui::tool_display::elapsed_label(3725), "1h 02m");
}

TEST(tool_display_working_label) {
    std::string w = tui::tool_display::working_label("◐", 12);
    ASSERT(w.find("◐") == 0);
    ASSERT(w.find("working") != std::string::npos);
    ASSERT(w.find("12s") != std::string::npos);
}

TEST(tool_display_working_label_with_task) {
    std::string w = tui::tool_display::working_label(
        "◐", 12, "grep -rn CancellationToken src/");
    ASSERT(w.find("◐") == 0);
    ASSERT(w.find("working") != std::string::npos);
    ASSERT(w.find("12s") != std::string::npos);
    ASSERT(w.find("· grep -rn CancellationToken src/") != std::string::npos);
}

TEST(tool_display_working_label_task_truncated) {
    std::string long_task(80, 'x');
    std::string w = tui::tool_display::working_label("◐", 5, long_task);
    size_t pos = w.find("· ");
    ASSERT(pos != std::string::npos);
    std::string shown = w.substr(pos + std::string("· ").size());
    ASSERT(tui::text::display_cols(shown) <= 40);
    ASSERT(shown.find("…") != std::string::npos);
}

TEST(tool_display_working_label_task_omitted_when_empty) {
    std::string w = tui::tool_display::working_label("◐", 5, "");
    ASSERT(w.find("·") == std::string::npos);
}

TEST(tool_display_reasoning_badge_mapping) {
    // The badge composes INSIDE the model bracket: [model(high)].
    ASSERT_EQ(tui::tool_display::reasoning_badge("off"), "(off)");
    ASSERT_EQ(tui::tool_display::reasoning_badge("low"), "(low)");
    ASSERT_EQ(tui::tool_display::reasoning_badge("medium"), "(medium)");
    ASSERT_EQ(tui::tool_display::reasoning_badge("high"), "(high)");
    ASSERT_EQ(tui::tool_display::reasoning_badge("turbo"), "(turbo)");
}

namespace {

std::string join_runs(const tui::rich::Line& ln) {
    std::string out;
    for (const auto& r : ln.runs) out += r.text;
    return out;
}

} // namespace

TEST(tool_display_result_line_shows_command) {
    auto ln = tui::tool_display::result_line(
        "bash", agent::json{{"command", "grep -rn Foo src/"}}, true,
        "line1\nline2\n", "");
    std::string t = join_runs(ln);
    ASSERT(t.find("grep -rn Foo src/") != std::string::npos);
    ASSERT(t.find("bash") == std::string::npos);
    // The icon conveys success — the redundant "exit 0" text is gone.
    ASSERT(t.find("exit 0") == std::string::npos);
    ASSERT(t.find("3 lines") != std::string::npos);
    ASSERT(!ln.runs.empty());
    ASSERT_EQ(ln.runs[0].pair, tui::P_GIT_PLUS);
}

TEST(tool_display_result_line_error_path) {
    auto ln = tui::tool_display::result_line(
        "read", agent::json{{"path", "include/agent/config.h"}}, false, "",
        "permission denied");
    std::string t = join_runs(ln);
    ASSERT(t.find("read include/agent/config.h") != std::string::npos);
    ASSERT(t.find("error: permission denied") != std::string::npos);
    ASSERT_EQ(ln.runs[0].pair, tui::P_GIT_MINUS);
}

TEST(tool_display_result_line_preview_truncated) {
    std::string big(200, 'x');
    auto ln = tui::tool_display::result_line(
        "bash", agent::json{{"command", "ls"}}, true, big, "");
    ASSERT(join_runs(ln).find("...") != std::string::npos);
}

TEST(tool_display_result_line_shows_lines_tail) {
    auto ln = tui::tool_display::result_line(
        "bash", agent::json{{"command", "ls -la"}}, true, "a\nb\nc\n", "");
    std::string t = join_runs(ln);
    ASSERT(t.find("ls -la") != std::string::npos);
    ASSERT(t.find("4 lines") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Approval dialog model (timeout fail-safe + countdown)
// ---------------------------------------------------------------------------

namespace {
double fake_now = 0.0;
tui::ApprovalModel make_model(int timeout_sec, int default_idx) {
    fake_now = 0.0;
    return tui::ApprovalModel(timeout_sec, default_idx,
                              []() { return fake_now; });
}
} // namespace

TEST(approval_timeout_auto_denies) {
    auto m = make_model(2, 0);  // 2s timeout, default selection AllowOnce
    fake_now = 2.0;
    m.poll();
    ASSERT(m.timed_out());
    // Fail-safe: a timed-out dialog must DENY, never auto-confirm the default.
    ASSERT(m.resolve(m.selection()) == agent::Approval::Deny);
}

TEST(approval_timeout_zero_waits_forever) {
    auto m = make_model(0, 0);
    fake_now = 1000.0;
    m.poll();
    ASSERT(!m.timed_out());
}

TEST(approval_verdict_mapping) {
    auto m = make_model(0, 0);
    ASSERT(m.resolve(0) == agent::Approval::AllowOnce);
    ASSERT(m.resolve(1) == agent::Approval::AllowSession);
    ASSERT(m.resolve(2) == agent::Approval::AlwaysAllow);
    ASSERT(m.resolve(3) == agent::Approval::AlwaysDeny);
    ASSERT(m.resolve(-1) == agent::Approval::Deny);  // Esc / cancel
}

TEST(approval_selection_clamps) {
    auto m = make_model(0, 0);
    m.select(99);
    ASSERT_EQ(m.selection(), 3);
    m.select(-5);
    ASSERT_EQ(m.selection(), 0);
    m.select(2);
    ASSERT_EQ(m.selection(), 2);
}

TEST(approval_countdown_decrements) {
    auto m = make_model(5, 0);
    fake_now = 2.0;
    m.poll();
    ASSERT_EQ(m.remaining_sec(), 3);
    fake_now = 6.0;
    m.poll();
    ASSERT(m.timed_out());
    ASSERT_EQ(m.remaining_sec(), 0);
}

// ---------------------------------------------------------------------------
// Setting value parsing (numeric setters must never throw)
// ---------------------------------------------------------------------------

TEST(parse_setting_int_accepts_valid) {
    auto v = tui::text::parse_setting_int("42", 0, 999);
    ASSERT(v.has_value());
    ASSERT_EQ(*v, 42);
    ASSERT_EQ(*tui::text::parse_setting_int("0", 0, 999), 0);
    ASSERT_EQ(*tui::text::parse_setting_int("999", 0, 999), 999);
}

TEST(parse_setting_int_rejects_garbage) {
    ASSERT(!tui::text::parse_setting_int("abc", 0, 999).has_value());
    ASSERT(!tui::text::parse_setting_int("", 0, 999).has_value());
    ASSERT(!tui::text::parse_setting_int("12x", 0, 999).has_value());
}

TEST(parse_setting_int_rejects_out_of_range) {
    ASSERT(!tui::text::parse_setting_int("-1", 0, 999).has_value());
    ASSERT(!tui::text::parse_setting_int("1000", 0, 999).has_value());
    ASSERT(!tui::text::parse_setting_int("5", 1, 4).has_value());
}

TEST(parse_setting_double_accepts_valid) {
    auto v = tui::text::parse_setting_double("0.5", 0.1, 1.0);
    ASSERT(v.has_value());
    ASSERT(v.value() > 0.49 && v.value() < 0.51);
}

TEST(parse_setting_double_rejects_garbage) {
    ASSERT(!tui::text::parse_setting_double("x", 0.1, 1.0).has_value());
    ASSERT(!tui::text::parse_setting_double("", 0.1, 1.0).has_value());
    ASSERT(!tui::text::parse_setting_double("2.0", 0.1, 1.0).has_value());
}

// ---------------------------------------------------------------------------
// Signal state (async-signal-safe shutdown flag)
// ---------------------------------------------------------------------------

TEST(signal_state_consume_once) {
    tui::SignalState s;
    s.raise();
    s.raise();
    ASSERT(s.consume());
    ASSERT(!s.consume());
}

// ---------------------------------------------------------------------------
// Event routing (window-stamped events) + approval deny-on-shutdown
// ---------------------------------------------------------------------------

TEST(route_event_targets_stamped_window) {
    std::vector<std::unique_ptr<tui::Window>> windows;
    windows.push_back(std::make_unique<tui::Window>());
    windows.push_back(std::make_unique<tui::Window>());
    tui::AgentEvent ev;
    ev.window_id = 1;
    ASSERT(route_event(windows, ev, 0) == windows[1].get());
}

TEST(route_event_drops_unknown_window) {
    std::vector<std::unique_ptr<tui::Window>> windows;
    windows.push_back(std::make_unique<tui::Window>());
    tui::AgentEvent ev;
    ev.window_id = 7;
    ASSERT(route_event(windows, ev, 0) == nullptr);
}

TEST(route_event_npos_routes_to_active) {
    std::vector<std::unique_ptr<tui::Window>> windows;
    windows.push_back(std::make_unique<tui::Window>());
    windows.push_back(std::make_unique<tui::Window>());
    tui::AgentEvent ev;
    ev.window_id = std::string::npos;
    ASSERT(route_event(windows, ev, 1) == windows[1].get());
}

TEST(deny_all_pending_approvals_resolves_all) {
    std::queue<tui::AgentEvent> q;
    for (int i = 0; i < 3; ++i) {
        tui::AgentEvent ev;
        ev.type = tui::AgentEvent::Approval;
        ev.approval_promise =
            std::make_shared<std::promise<agent::Approval>>();
        q.push(std::move(ev));
    }
    std::vector<std::future<agent::Approval>> futures;
    std::queue<tui::AgentEvent> copy = q;
    while (!copy.empty()) {
        futures.push_back(copy.front().approval_promise->get_future());
        copy.pop();
    }
    deny_all_pending_approvals(q);
    ASSERT(q.empty());
    for (auto& f : futures) {
        ASSERT(f.get() == agent::Approval::Deny);
    }
}
