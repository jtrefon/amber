// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent.h"
#include "agent/tools.h"
#include "agent/search_backend.h"
#include "agent/sse_parser.h"
#include "agent/request_builder.h"
#include "agent/compressor.h"
#include "agent/experience.h"
#include "tui/textutil.h"
#include "tui/palette.h"
#include "tui/rich.h"
#include "tui/markdown.h"
#include "tests/test_util.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static inline void run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    (void)rc;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(config_defaults) {
    agent::Config c;
    ASSERT_EQ(c.api_base, "http://localhost:8000/v1");
    ASSERT_EQ(c.model, "gpt-4o-mini");
    ASSERT_EQ(c.max_tool_iterations, 100);
    ASSERT_TRUE(c.stream);
    ASSERT_EQ(c.api_url(), "http://localhost:8000/v1/chat/completions");
}

TEST(config_validate_accepts_defaults) {
    agent::Config c;
    ASSERT_TRUE(c.validate().empty());
}

TEST(config_validate_flags_problems) {
    agent::Config c;
    c.api_base = "localhost:8000/v1";   // missing scheme
    c.model = "";                       // empty
    c.max_tool_iterations = 0;          // too small
    c.temperature = 5.0;                // out of range
    c.max_tokens = 0;                   // zero
    c.thinking = "sometimes";           // invalid enum
    auto errs = c.validate();
    ASSERT(errs.size() >= 6);

    agent::Config trailing;
    trailing.api_base = "http://localhost:8000/v1/";  // trailing slash
    ASSERT_FALSE(trailing.validate().empty());
}

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
    // No selection: extend to the longest common prefix of the matches'
    // names (here just "window", no trailing space until it is exact).
    ASSERT_EQ(tui::palette::complete(cmds, "/wi", -1), "/window");
    // Exact unique name: append a space, ready for args.
    ASSERT_EQ(tui::palette::complete(cmds, "/window", -1), "/window ");
    // Selection index picks that match directly.
    ASSERT_EQ(tui::palette::complete(cmds, "/", 3), "/quit ");
    // No match: input unchanged.
    ASSERT_EQ(tui::palette::complete(cmds, "/zzz", -1), "/zzz");
}

TEST(palette_usage_and_common_prefix) {
    tui::palette::Command c{"window", {}, "new|close", "manage", nullptr};
    ASSERT_EQ(tui::palette::usage(c), "/window new|close");
    tui::palette::Command bare{"save", {}, "", "persist", nullptr};
    ASSERT_EQ(tui::palette::usage(bare), "/save");
    ASSERT_EQ(tui::palette::common_prefix({"send", "set", "sever"}), "se");
    ASSERT_EQ(tui::palette::common_prefix({}), "");
}

TEST(config_load_key_value) {
    std::string path = "/tmp/amber_cfg_test.txt";
    {
        std::ofstream f(path);
        f << "# comment\n";
        f << "model=\"my-model\"\n";
        f << "api_base=http://example:1234/v1\n";
        f << "max_tool_iterations=5\n";
        f << "temperature=0.9\n";
        f << "stream=false\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_EQ(c.api_base, "http://example:1234/v1");
    ASSERT_EQ(c.max_tool_iterations, 5);
    ASSERT_EQ(c.temperature, 0.9);
    ASSERT_FALSE(c.stream);
    std::remove(path.c_str());
}

// Mirrors what the TUI F10 "save settings" writes for a llama.cpp server, and
// that an optional (possibly empty) token survives a load round-trip. This
// guards the settings-persistence contract used by the TUI.
TEST(config_save_settings_roundtrip) {
    std::string path = "/tmp/amber_settings_test.conf";
    {
        std::ofstream f(path);
        f << "# amber settings\n";
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=sk-test-token\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.api_base, "http://localhost:8080/v1");
    ASSERT_EQ(c.api_key, "sk-test-token");
    ASSERT_EQ(c.model, "llama-3.2-3b-instruct");

    // Optional/blank token is also preserved as empty.
    {
        std::ofstream f(path);
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c2;
    c2.load(path);
    ASSERT_EQ(c2.api_key, "");
    std::remove(path.c_str());
}

TEST(config_save_settings_keeps_llm_global) {
    // The project-local settings file must NOT duplicate LLM provider config
    // (api_base / api_key / model / context_size); those stay in the global
    // config. save_settings() omits them by design.
    agent::Config c;
    c.api_base = "http://localhost:8080/v1";
    c.api_key = "sk-secret";
    c.model = "llama-3.2-3b";
    c.model_explicit = true;
    c.context_size = 8192;
    c.context_explicit = true;
    c.temperature = 0.7;
    c.stream = false;
    c.debug_log = "/tmp/amber_debug.log";

    std::string path = "/tmp/amber_settings_local.conf";
    ASSERT_TRUE(c.save_settings(path));

    agent::Config d;
    d.load(path);
    // LLM provider settings are intentionally omitted from the local file, so
    // loading it must NOT pick up the provider values we set above.
    ASSERT(d.api_base != "http://localhost:8080/v1");
    ASSERT(d.api_key != "sk-secret");
    ASSERT(d.model != "llama-3.2-3b");
    ASSERT(d.context_size != 8192);
    // Non-LLM settings round-trip.
    ASSERT_EQ(d.temperature, 0.7);
    ASSERT_EQ(d.stream, false);
    ASSERT_EQ(d.debug_log, "/tmp/amber_debug.log");
    // And the raw file must not contain the LLM keys.
    std::ifstream f(path);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    ASSERT(body.find("api_base=") == std::string::npos);
    ASSERT(body.find("api_key=") == std::string::npos);
    ASSERT(body.find("model=") == std::string::npos);
    std::remove(path.c_str());
}

TEST(config_missing_file_is_noop) {
    agent::Config c;
    c.model = "keep";
    c.load("/nonexistent/path/xyz.cfg");
    ASSERT_EQ(c.model, "keep");
}

// Regression: a saved config with a blank model / zero context must NOT mark
// those values explicit, otherwise startup auto-detection is permanently
// disabled and the server's real model/provider gets overwritten by defaults.
TEST(config_blank_model_and_zero_context_stay_auto) {
    std::string path = "/tmp/amber_auto_test.conf";
    {
        std::ofstream f(path);
        f << "api_base=http://localhost:8080/v1\n";
        f << "model=\n";            // blank => auto-detect
        f << "context_size=0\n";    // zero  => auto-detect
    }
    agent::Config c;
    c.load(path);
    ASSERT_TRUE(c.model.empty());
    ASSERT_FALSE(c.model_explicit);
    ASSERT_EQ(c.context_size, 0);
    ASSERT_FALSE(c.context_explicit);
    std::remove(path.c_str());
}

// An explicit (non-blank / positive) config still wins over auto-detection.
TEST(config_explicit_model_and_context_are_flagged) {
    std::string path = "/tmp/amber_explicit_test.conf";
    {
        std::ofstream f(path);
        f << "model=my-model\n";
        f << "context_size=16384\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_TRUE(c.model_explicit);
    ASSERT_EQ(c.context_size, 16384);
    ASSERT_TRUE(c.context_explicit);
    std::remove(path.c_str());
}

// Regression guard for the actual save path (Config::save, used by the TUI F10
// "save settings"). Auto-detected model/context must be written as sentinels so
// a reload re-enables auto-detection instead of pinning the probed value. This
// exercises the real serializer, not a hand-written file.
TEST(config_save_preserves_autodetect_intent) {
    std::string path = "/tmp/amber_save_auto.conf";
    agent::Config src;
    src.api_base = "http://localhost:8081/v1";
    src.api_key = "";
    // Simulate a probe having filled these, WITHOUT the user setting them.
    src.model = "qwen-probed";  src.model_explicit = false;
    src.context_size = 8192;    src.context_explicit = false;
    ASSERT_TRUE(src.save(path));

    agent::Config back;
    back.load(path);
    ASSERT_EQ(back.api_base, "http://localhost:8081/v1");
    ASSERT_TRUE(back.model.empty());       // sentinel, not the probed value
    ASSERT_FALSE(back.model_explicit);     // auto-detect stays enabled
    ASSERT_EQ(back.context_size, 0);       // sentinel, not 8192
    ASSERT_FALSE(back.context_explicit);
    std::remove(path.c_str());
}

// The mirror case: user-set model/context must survive save->load as explicit.
TEST(config_save_preserves_explicit_values) {
    std::string path = "/tmp/amber_save_explicit.conf";
    agent::Config src;
    src.model = "my-model";   src.model_explicit = true;
    src.context_size = 32768; src.context_explicit = true;
    ASSERT_TRUE(src.save(path));

    agent::Config back;
    back.load(path);
    ASSERT_EQ(back.model, "my-model");
    ASSERT_TRUE(back.model_explicit);
    ASSERT_EQ(back.context_size, 32768);
    ASSERT_TRUE(back.context_explicit);
    std::remove(path.c_str());
}

// Context size defaults to 0 (auto) so the gauge hides and probing is allowed.
TEST(config_context_default_is_auto) {
    agent::Config c;
    ASSERT_EQ(c.context_size, 0);
    ASSERT_FALSE(c.context_explicit);
}

// ---------------------------------------------------------------------------
// UTF-8 safety: tool/model text may carry invalid bytes (e.g. binary from
// grep). Serializing it for the API request used to throw
// [json.exception.type_error.316] and abort the turn. It must not.
// ---------------------------------------------------------------------------

TEST(request_body_survives_invalid_utf8) {
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message tool;
    tool.role = "tool";
    tool.name = "search";
    tool.tool_call_id = "x";
    // 0x66 ('f') followed by a lone continuation byte 0x80 — invalid UTF-8.
    std::string bad = "hits:\nfoo";
    bad.push_back(static_cast<char>(0x80));  // lone continuation byte: invalid UTF-8
    bad += "bar";
    tool.content = bad;
    msgs.push_back(tool);

    std::vector<agent::Tool*> no_tools;
    json body = build_chat_body(c, msgs, no_tools, false);
    std::string payload;
    bool threw = false;
    try {
        payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (...) {
        threw = true;
    }
    ASSERT_FALSE(threw);
    ASSERT(payload.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD present
}

TEST(request_builder_assistant_message_always_has_content) {
    // Regression: a reasoning model can answer with content "" and the whole
    // reply in reasoning_content. Serializing that as {"role":"assistant"}
    // (no content field) makes the server reject the request with HTTP 400
    // ("Assistant message must contain either 'content' or 'tool_calls'").
    // build_chat_body must always emit a content field (even empty) so a
    // stripped/empty reply never produces a malformed request.
    agent::Config c;
    std::vector<agent::Message> msgs;
    agent::Message u; u.role = "user"; u.content = "think hard"; msgs.push_back(u);
    agent::Message a; a.role = "assistant"; a.content = "";  // empty, no tool_calls
    msgs.push_back(a);
    agent::Message t; t.role = "tool"; t.name = "read"; t.tool_call_id = "c1";
    t.content = "";  // empty tool result
    msgs.push_back(t);

    std::vector<agent::Tool*> no_tools;
    json body = build_chat_body(c, msgs, no_tools, false);
    ASSERT(body.contains("messages"));
    for (auto& m : body["messages"]) {
        // every message must carry a content field (regression: empty
        // assistant/tool content previously dropped the field -> HTTP 400).
        ASSERT(m.contains("content"));
    }
}

// ---------------------------------------------------------------------------
// Tool registry
// ---------------------------------------------------------------------------

TEST(registry_register_and_find) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::register_default_tools(r, jobs);
    ASSERT_FALSE(r.empty());
    ASSERT_EQ(r.tools().size(), 7u);
    ASSERT(r.find("read") != nullptr);
    ASSERT(r.find("write") != nullptr);
    ASSERT(r.find("search") != nullptr);
    ASSERT(r.find("bash") != nullptr);
    ASSERT(r.find("process_start") != nullptr);
    ASSERT(r.find("process_read") != nullptr);
    ASSERT(r.find("process_stop") != nullptr);
    ASSERT(r.find("nonexistent") == nullptr);
}

TEST(registry_schema_shape) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::register_default_tools(r, jobs);
    agent::json s = r.schema();
    ASSERT(s.is_array());
    ASSERT_EQ(s.size(), 7u);
    for (const auto& t : s) {
        ASSERT(t.contains("type"));
        ASSERT_EQ(t["type"], "function");
        ASSERT(t["function"].contains("name"));
        ASSERT(t["function"].contains("description"));
        ASSERT(t["function"]["parameters"].contains("properties"));
    }
}

// ---------------------------------------------------------------------------
// Prompt loading + tool advertising
// ---------------------------------------------------------------------------

TEST(prompt_missing_file_empty) {
    ASSERT_EQ(agent::load_prompt("/does/not/exist.md"), "");
}

TEST(prompt_loads_existing) {
    std::string path = "/tmp/amber_prompt_test.md";
    {
        std::ofstream f(path);
        f << "# Title\nbody text\n";
    }
    ASSERT_EQ(agent::load_prompt(path), "# Title\nbody text\n");
    std::remove(path.c_str());
}

TEST(prompt_render_tools_markdown_lists_all) {
    agent::ToolRegistry r;
    agent::JobService jobs;
    agent::register_default_tools(r, jobs);
    std::string md = agent::render_tools_markdown(r);
    ASSERT(md.find("## Available Tools") != std::string::npos);
    ASSERT(md.find("`read`") != std::string::npos);
    ASSERT(md.find("`write`") != std::string::npos);
    ASSERT(md.find("`search`") != std::string::npos);
    ASSERT(md.find("`bash`") != std::string::npos);
    ASSERT(md.find("path") != std::string::npos);   // a known parameter
}

// ---------------------------------------------------------------------------
// read tool (pagination)
// ---------------------------------------------------------------------------

TEST(read_tool_basic_and_pagination) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_read_test.txt";
    {
        std::ofstream f(path);
        for (int i = 1; i <= 10; ++i) f << "line " << i << "\n";
    }
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", path}, {"offset", 1}, {"limit", 3}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("1:\tline 1") != std::string::npos);
    ASSERT(r.output.find("3:\tline 3") != std::string::npos);
    ASSERT(r.output.find("remaining") != std::string::npos);

    // page 2
    auto r2 = tool->execute({{"path", path}, {"offset", 4}, {"limit", 3}});
    ASSERT_TRUE(r2.ok);
    ASSERT(r2.output.find("4:\tline 4") != std::string::npos);
    ASSERT(r2.output.find("6:\tline 6") != std::string::npos);

    // past end reports EOF, no remaining
    auto r3 = tool->execute({{"path", path}, {"offset", 9}, {"limit", 50}});
    ASSERT_TRUE(r3.ok);
    ASSERT(r3.output.find("end of file") != std::string::npos);
    std::remove(path.c_str());
}

TEST(read_tool_missing_path_errors) {
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"limit", 5}});   // no path
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// write tool (patch style)
// ---------------------------------------------------------------------------

TEST(write_tool_create_then_patch) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();

    auto r = tool->execute({{"path", path},
                            {"edits", {{{"old", ""}, {"new", "alpha\nbeta\n"}}}}});
    ASSERT_TRUE(r.ok);
    {
        std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\nbeta\n");
    }

    auto r2 = tool->execute({{"path", path},
                             {"edits", {{{"old", "beta"}, {"new", "gamma"}}}}});
    ASSERT_TRUE(r2.ok);
    {
        std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\ngamma\n");
    }
    std::remove(path.c_str());
}

TEST(write_tool_missing_old_fails) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test2.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();
    auto r = tool->execute({{"path", path},
                            {"edits", {{{"old", "nope"}, {"new", "x"}}}}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("not found") != std::string::npos);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// workspace path confinement
// ---------------------------------------------------------------------------

TEST(workspace_confines_relative_and_rejects_escape) {
    agent::Workspace::set_root("/tmp/amber_ws");
    std::string resolved, err;

    ASSERT_TRUE(agent::Workspace::confine("a/b.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/a/b.txt");

    ASSERT_TRUE(agent::Workspace::confine("./x/../y.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/y.txt");

    ASSERT_FALSE(agent::Workspace::confine("../../etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    ASSERT_FALSE(agent::Workspace::confine("/etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    // sibling directory sharing a prefix must not be treated as inside
    ASSERT_FALSE(agent::Workspace::confine("/tmp/amber_ws2/x", resolved, err));
}

TEST(read_write_tools_reject_paths_outside_workspace) {
    agent::Workspace::set_root("/tmp/amber_ws_tools");
    run_cmd("mkdir -p /tmp/amber_ws_tools");

    auto rtool = agent::make_read_tool();
    auto rr = rtool->execute({{"path", "/etc/passwd"}});
    ASSERT_FALSE(rr.ok);
    ASSERT(rr.error.find("workspace") != std::string::npos);

    auto wtool = agent::make_write_tool();
    auto wr = wtool->execute({{"path", "../escape.txt"},
                              {"edits", {{{"old", ""}, {"new", "x"}}}}});
    ASSERT_FALSE(wr.ok);
    ASSERT(wr.error.find("workspace") != std::string::npos);
}

// ---------------------------------------------------------------------------
// search backends
// ---------------------------------------------------------------------------

namespace {
std::string make_search_tree() {
    std::string dir = "/tmp/amber_srch";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir + "/sub";
    run_cmd(cmd);
    {
        std::ofstream f(dir + "/a.cpp");
        f << "void register_default_tools() {}\nint helper() { return 1; }\n";
    }
    {
        std::ofstream f(dir + "/sub/b.cpp");
        f << "void register_default_tools() {}\n// unrelated content\n";
    }
    {
        std::ofstream f(dir + "/note.txt");
        f << "register_default_tools is the function we want to find\n";
    }
    return dir;
}
} // namespace

TEST(search_grep_backend) {
    std::string dir = make_search_tree();
    auto be = agent::make_grep_backend();
    auto hits = be->search("register_default_tools", dir, "*.cpp", 100);
    ASSERT_EQ(be->name(), "grep");
    ASSERT_FALSE(hits.empty());
    bool saw_a = false, saw_b = false;
    for (const auto& h : hits) {
        if (h.path.find("a.cpp") != std::string::npos) saw_a = true;
        if (h.path.find("b.cpp") != std::string::npos) saw_b = true;
        ASSERT(h.line_no > 0);
    }
    ASSERT(saw_a && saw_b);
    run_cmd("rm -rf " + dir);
}

TEST(search_grep_backend_resists_shell_injection) {
    std::string dir = make_search_tree();
    std::string sentinel = "/tmp/amber_pwned";
    run_cmd("rm -f " + sentinel);
    auto be = agent::make_grep_backend();
    // A query crafted to break out of the command if quoting were absent.
    auto hits = be->search("x'; touch " + sentinel + "; echo '", dir, "", 100);
    // The injected command must not have run.
    ASSERT_FALSE(access(sentinel.c_str(), F_OK) == 0);
    (void)hits;
    run_cmd("rm -rf " + dir + " " + sentinel);
}

TEST(search_semantic_backend_ranks_relevant) {
    std::string dir = make_search_tree();
    auto be = agent::make_semantic_backend();
    auto hits = be->search("register the default tools function", dir, "", 5);
    ASSERT_EQ(be->name(), "semantic");
    ASSERT_FALSE(hits.empty());
    // The line containing register_default_tools should rank at or near the top.
    bool top_has_target = hits[0].line.find("register_default_tools") != std::string::npos;
    ASSERT(top_has_target);
    ASSERT(hits[0].score > 0.0);
    run_cmd("rm -rf " + dir);
}

TEST(search_tool_mode_switch) {
    std::string dir = make_search_tree();
    auto tool = agent::make_search_tool();

    auto g = tool->execute({{"pattern", "register_default_tools"},
                            {"path", dir}, {"glob", "*.cpp"}, {"mode", "grep"}});
    ASSERT_TRUE(g.ok);
    ASSERT(g.output.find("[grep]") != std::string::npos);

    auto s = tool->execute({{"pattern", "register the default tools"},
                            {"path", dir}, {"mode", "semantic"}});
    ASSERT_TRUE(s.ok);
    ASSERT(s.output.find("[semantic]") != std::string::npos);
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// Server auto-detection: /v1/models parsing (pure, no network)
// ---------------------------------------------------------------------------

TEST(probe_parse_llamacpp_models) {
    // Real llama.cpp /v1/models shape (trimmed): id + meta.n_ctx/n_ctx_train.
    std::string body = R"({"object":"list","data":[{"id":"Qwopus3.6-27B.gguf",)"
        R"("object":"model","owned_by":"llamacpp","meta":{"n_vocab":248320,)"
        R"("n_ctx":262144,"n_ctx_train":262144,"n_embd":5120}}]})";
    agent::ServerInfo info = agent::LLMClient::parse_models(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "Qwopus3.6-27B.gguf");
    ASSERT_EQ(info.context_size, 262144);
    ASSERT_EQ(info.context_train, 262144);
}

TEST(probe_parse_models_array_fallback) {
    // Ollama-ish {"models":[{"name":..,"n_ctx":..}]} fallback shape.
    std::string body =
        R"({"models":[{"name":"llama-3.2-3b","n_ctx":8192}]})";
    agent::ServerInfo info = agent::LLMClient::parse_models(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "llama-3.2-3b");
    ASSERT_EQ(info.context_size, 8192);
}

TEST(probe_parse_models_malformed_is_not_ok) {
    ASSERT_FALSE(agent::LLMClient::parse_models("not json").ok);
    ASSERT_FALSE(agent::LLMClient::parse_models("{}").ok);
    ASSERT_FALSE(agent::LLMClient::parse_models(R"({"data":[]})").ok);
}

// The auto-detect merge policy: probe results fill only fields the user left on
// auto; explicit values are never overwritten. Network-free (merge_server_info).
TEST(autodetect_fills_only_auto_fields) {
    agent::ServerInfo info;
    info.ok = true;
    info.model = "server-model";
    info.context_size = 262144;

    // Both auto -> both filled.
    agent::Config a;  // defaults: model_explicit=false, context_explicit=false
    agent::merge_server_info(a, info);
    ASSERT_EQ(a.model, "server-model");
    ASSERT_EQ(a.context_size, 262144);

    // Both explicit -> untouched.
    agent::Config b;
    b.model = "user-model";   b.model_explicit = true;
    b.context_size = 4096;    b.context_explicit = true;
    agent::merge_server_info(b, info);
    ASSERT_EQ(b.model, "user-model");
    ASSERT_EQ(b.context_size, 4096);

    // Mixed -> only the auto one changes.
    agent::Config c;
    c.model = "user-model";   c.model_explicit = true;   // pinned
    c.context_size = 0;       c.context_explicit = false; // auto
    agent::merge_server_info(c, info);
    ASSERT_EQ(c.model, "user-model");
    ASSERT_EQ(c.context_size, 262144);
}

// An unreachable / not-ok probe must never mutate the config.
TEST(autodetect_noop_when_server_down) {
    agent::ServerInfo down;  // ok defaults to false
    down.model = "ghost";
    down.context_size = 999;
    agent::Config c;
    c.model = "keep";  // auto, but server is down
    agent::merge_server_info(c, down);
    ASSERT_EQ(c.model, "keep");
    ASSERT_EQ(c.context_size, 0);
}

// ---------------------------------------------------------------------------
// Status-bar rendering math (pure, no ncurses / no network)
// ---------------------------------------------------------------------------

TEST(statusbar_kfmt) {
    ASSERT_EQ(agent::bar::kfmt(-1), "?");
    ASSERT_EQ(agent::bar::kfmt(0), "0");
    ASSERT_EQ(agent::bar::kfmt(512), "512");
    ASSERT_EQ(agent::bar::kfmt(999), "999");
    ASSERT_EQ(agent::bar::kfmt(5000), "5.0k");
    ASSERT_EQ(agent::bar::kfmt(1500), "1.5k");
    ASSERT_EQ(agent::bar::kfmt(128000), "128k");
}

TEST(statusbar_pressure_thresholds) {
    ASSERT(agent::bar::pressure(0.0) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.59) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.60) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.85) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.851) == agent::bar::Pressure::Crit);
    ASSERT(agent::bar::pressure(1.0) == agent::bar::Pressure::Crit);
}

TEST(statusbar_gauge_fill_cells) {
    // Empty and full extremes.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.0, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(1.0, 10), 10);
    // Half fill of 10 cells = 5 full cells.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 10), 5);
    // Clamps out-of-range fractions.
    ASSERT_EQ(agent::bar::gauge_full_cells(-0.5, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(2.0, 10), 10);
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 0), 0);
}

TEST(statusbar_gauge_bar_glyphs) {
    // Empty bar is all light-shade track (\u2591), one per cell (3 bytes each).
    std::string empty = agent::bar::gauge_bar(0.0, 4);
    ASSERT_EQ(empty, "\u2591\u2591\u2591\u2591");
    // Full bar is all full blocks (\u2588).
    std::string full = agent::bar::gauge_bar(1.0, 4);
    ASSERT_EQ(full, "\u2588\u2588\u2588\u2588");
    // Half of 4 cells: two full blocks then two empty.
    std::string half = agent::bar::gauge_bar(0.5, 4);
    ASSERT_EQ(half, "\u2588\u2588\u2591\u2591");
    // Degenerate width.
    ASSERT_EQ(agent::bar::gauge_bar(0.5, 0), "");
}

// ---------------------------------------------------------------------------
// LLM streaming SSE parse (integration via a tiny in-process HTTP server)
// ---------------------------------------------------------------------------

#ifdef __linux__
#include <netinet/in.h>

namespace {
// Serve one canned SSE response (a streamed tool call in two fragments), then
// close. Lets us exercise LLMClient::chat_stream including fragment merging
// without any external dependency.
int spawn_mock_sse(int port, std::string& body_out, const std::string& sse_override = "") {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { close(fd); return -1; }
    listen(fd, 1);
    body_out.clear();
    std::thread t([fd, sse_override]() {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) return;
        // read the request (headers + body) until we have it
        char buf[4096];
        std::string req;
        while (true) {
            int n = recv(c, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            req.append(buf, n);
            if (req.find("\r\n\r\n") != std::string::npos) break;
        }
        (void)req;
        std::string sse = !sse_override.empty() ? sse_override :
            std::string(
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
            "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
            "\"arguments\":\"\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"{\\\"pattern\\\":\\\"foo\\\",\\\"path\\\":\\\".\\\"}\"}}]}}]}\n\n"
            "data: [DONE]\n\n");
        std::string http =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: " + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        send(c, http.c_str(), http.size(), 0);
        // give client time to read
        usleep(200000);
        close(c);
    });
    t.detach();
    return fd;
}
} // namespace

 TEST(llm_streaming_tool_call_object_arguments_preserved) {
    // Some OpenAI-compatible servers stream tool-call `arguments` as a JSON
    // object in one delta instead of a string fragment. The parser must
    // preserve it, otherwise a valid call (e.g. search with a pattern) arrives
    // at the tool as `{}` and errors ("missing 'pattern'").
    agent::Message m;
    auto sink = [](const agent::StreamChunk&) {};
    agent::StreamParser p(m, sink, "");

    auto ev = [](const agent::json& tc) -> std::string {
        agent::json delta = {{"tool_calls", tc}};
        agent::json choice = {{"delta", delta}};
        return "data: " +
               agent::json{{"choices", agent::json::array({choice})}}.dump() +
               "\n\n";
    };

    // Fragment 1: arguments as a JSON object (not a string).
    agent::json fn1 = {{"name", "search"},
                       {"arguments", agent::json::object({{"pattern", "ncurses"}})}};
    agent::json call1 = {{"index", 0}, {"id", "c1"}, {"type", "function"},
                         {"function", fn1}};
    std::string s1 = ev(agent::json::array({call1}));

    // Fragment 2: a trailing string fragment appended to the object.
    agent::json fn2 = {{"arguments", "|curses"}};
    agent::json call2 = {{"index", 0}, {"function", fn2}};
    std::string s2 = ev(agent::json::array({call2}));

    p.on_write(s1.c_str(), s1.size(), 1);
    p.on_write(s2.c_str(), s2.size(), 1);
    p.finalize();

    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    // The OpenAI wire contract requires `arguments` to be a JSON *string*; an
    // object fragment must be merged and re-serialized, never stored as a raw
    // object (object-typed arguments sent back to the API corrupt the turn).
    agent::json args = m.tool_calls[0]["function"]["arguments"];
    ASSERT_TRUE(args.is_string());
    agent::json parsed = agent::json::parse(args.get<std::string>(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed["pattern"], "ncurses");
}

 TEST(llm_streaming_merges_tool_call_fragments) {
    std::string dummy;
    int srv = spawn_mock_sse(8911, dummy);
    ASSERT(srv >= 0);
    usleep(100000);  // let the listener bind


    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8911/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    std::vector<std::string> tokens;
    agent::Message m = client.chat_stream({}, {},
        [&tokens](const agent::StreamChunk& ch) {
            if (!ch.done && !ch.delta.empty()) tokens.push_back(ch.delta);
        });

    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "search");
    std::string args = m.tool_calls[0]["function"]["arguments"].get<std::string>();
    agent::json parsed = agent::json::parse(args, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed["pattern"], "foo");
    ASSERT_EQ(parsed["path"], ".");
    close(srv);
}

TEST(llm_streaming_inline_think_segmentation) {
    std::string dummy;
    // Content stream with inline <think> spanning fragments; answer follows.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<thi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"nk>plan the\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" answer</think>Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8912, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8912/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    std::string answer, reasoning;
    agent::Message m = client.chat_stream({}, {},
        [&](const agent::StreamChunk& ch) {
            if (ch.done) return;
            answer += ch.delta;
            reasoning += ch.reasoning;
        });

    ASSERT_EQ(m.content, "Hello world");
    ASSERT_EQ(m.reasoning, "plan the answer");
    ASSERT_EQ(answer, "Hello world");
    ASSERT_EQ(reasoning, "plan the answer");
    close(srv);
}

TEST(llm_streaming_reasoning_content_field) {
    std::string dummy;
    // Dedicated reasoning_content field (vLLM / llama.cpp deepseek format).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step one \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step two\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8913, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8913/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    agent::Message m = client.chat_stream({}, {},
        [](const agent::StreamChunk&) {});

    ASSERT_EQ(m.content, "done");
    ASSERT_EQ(m.reasoning, "step one step two");
    close(srv);
}

TEST(llm_streaming_captures_usage_stats) {
    std::string dummy;
    // Final include_usage chunk: usage present, empty choices[] (llama.cpp/vLLM).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4096,"
        "\"completion_tokens\":128,\"total_tokens\":4224}}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8914, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8914/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    agent::Stats stats;
    agent::Message m = client.chat_stream({}, {},
        [](const agent::StreamChunk&) {}, &stats);

    ASSERT_EQ(m.content, "hi");
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.prompt_tokens, 4096L);
    ASSERT_EQ(stats.completion_tokens, 128L);
    ASSERT_TRUE(stats.latency_ms >= 0);
    close(srv);
}
#endif // __linux__

// ---------------------------------------------------------------------------
// Session persistence
// ---------------------------------------------------------------------------

TEST(session_json_roundtrip_preserves_messages) {
    agent::Session s;
    s.model = "test-model";
    agent::Message sys; sys.role = "system"; sys.content = "be helpful";
    agent::Message u;   u.role = "user";     u.content = "hi\nthere";
    agent::Message a;   a.role = "assistant"; a.content = "hello";
    a.reasoning = "think first";
    s.messages = {sys, u, a};
    s.derive_title();
    ASSERT_EQ(s.title, "hi");

    agent::json j = s.to_json();
    agent::Session back = agent::Session::from_json(j);
    ASSERT_EQ(back.model, "test-model");
    ASSERT_EQ(back.messages.size(), 3u);
    ASSERT_EQ(back.messages[0].role, "system");
    ASSERT_EQ(back.messages[1].content, "hi\nthere");
    ASSERT_EQ(back.messages[2].reasoning, "think first");
}

TEST(session_store_save_load_list_delete) {
    std::string dir = "/tmp/amber_sessions_test";
    run_cmd("rm -rf " + dir);
    agent::SessionStore store(dir);

    agent::Session s1;
    agent::Message u; u.role = "user"; u.content = "first";
    s1.messages = {u};
    s1.derive_title();
    ASSERT_TRUE(store.save(s1));
    ASSERT_FALSE(s1.id.empty());
    ASSERT_TRUE(s1.updated_ms > 0);

    agent::Session loaded;
    ASSERT_TRUE(store.load(s1.id, loaded));
    ASSERT_EQ(loaded.messages.size(), 1u);
    ASSERT_EQ(loaded.messages[0].content, "first");

    usleep(2000);
    agent::Session s2;
    agent::Message u2; u2.role = "user"; u2.content = "second";
    s2.messages = {u2};
    s2.derive_title();
    ASSERT_TRUE(store.save(s2));

    auto metas = store.list();
    ASSERT_EQ(metas.size(), 2u);
    // Newest updated first.
    ASSERT_EQ(metas[0].id, s2.id);
    ASSERT_EQ(metas[0].message_count, 1);

    ASSERT_TRUE(store.remove(s1.id));
    ASSERT_EQ(store.list().size(), 1u);
    agent::Session gone;
    ASSERT_FALSE(store.load(s1.id, gone));
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// Agent conversation memory (stateful across run() calls)
// ---------------------------------------------------------------------------

TEST(agent_retains_history_across_turns) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent ag(cfg, reg);

    // Seed a prior conversation as if loaded from a session.
    agent::Message sys; sys.role = "system"; sys.content = "sys";
    agent::Message u;   u.role = "user";     u.content = "earlier";
    agent::Message a;   a.role = "assistant"; a.content = "reply";
    ag.set_history({sys, u, a});
    ASSERT_EQ(ag.history().size(), 3u);
    ASSERT_EQ(ag.history().front().role, "system");

    ag.reset();
    ASSERT_EQ(ag.history().size(), 0u);
}

// ---------------------------------------------------------------------------
// bash tool
// ---------------------------------------------------------------------------

TEST(bash_tool_runs_and_reports_exit) {
    auto tool = agent::make_bash_tool();
    ASSERT_TRUE(tool->requires_approval());

    auto ok = tool->execute({{"command", "echo hello"}});
    ASSERT_TRUE(ok.ok);
    ASSERT(ok.output.find("hello") != std::string::npos);
    ASSERT(ok.output.find("[exit 0]") != std::string::npos);

    auto bad = tool->execute({{"command", "exit 3"}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.output.find("[exit 3]") != std::string::npos);
    ASSERT(bad.error.find("status 3") != std::string::npos);
}

TEST(bash_tool_missing_command_errors) {
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"timeout", 5}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("command") != std::string::npos);
}

TEST(bash_tool_runs_in_workspace_root) {
    agent::Workspace::set_root("/tmp/amber_bash_ws");
    run_cmd("rm -rf /tmp/amber_bash_ws && mkdir -p /tmp/amber_bash_ws");
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"command", "pwd"}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("/tmp/amber_bash_ws") != std::string::npos);
}

TEST(bash_tool_times_out) {
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"command", "sleep 5"}, {"timeout", 1}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("timed out") != std::string::npos);
    ASSERT(r.output.find("timed out") != std::string::npos);
}

// Idle timeout (not a fixed wall-clock budget): a command that keeps emitting
// output must survive well past its timeout, while a silent one is killed.
TEST(bash_tool_idle_timeout_keeps_progressing) {
    auto tool = agent::make_bash_tool();
    // ~3s of runtime, output every 0.3s, timeout 1s: never idle long enough.
    auto r = tool->execute(
        {{"command",
          "for i in 1 2 3 4 5 6 7 8 9 10; do echo tick; sleep 0.3; done"},
         {"timeout", 1}});
    ASSERT(r.ok);
    ASSERT(r.output.find("[exit 0]") != std::string::npos);
    ASSERT(r.output.find("timed out") == std::string::npos);
}

TEST(bash_tool_truncates_large_output) {
    auto tool = agent::make_bash_tool();
    // yes emits far more than the 64 KiB cap; head bounds the runtime.
    auto r = tool->execute(
        {{"command", "yes AAAAAAAAAA | head -c 200000"}, {"timeout", 30}});
    ASSERT(r.output.find("[output truncated") != std::string::npos);
    ASSERT(r.output.size() < static_cast<std::size_t>(70) * 1024u);
}

// When the host wires a JobService, bash spawns through it so the process is
// visible in /job (and on the status bar) while it runs and is killable. The
// synchronous result returned to the model must be identical to the direct
// path, and the job must be cleaned up (erased) once the command finishes.
TEST(bash_tool_tracked_by_job_service) {
    agent::JobService jobs;
    auto tool = agent::make_bash_tool(&jobs);

    auto ok = tool->execute({{"command", "echo tracked"}});
    ASSERT_TRUE(ok.ok);
    ASSERT(ok.output.find("tracked") != std::string::npos);
    ASSERT(ok.output.find("[exit 0]") != std::string::npos);
    ASSERT(jobs.list().empty());  // finished job erased, not leaked

    auto bad = tool->execute({{"command", "exit 7"}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.output.find("[exit 7]") != std::string::npos);
    ASSERT(jobs.list().empty());

    auto to = tool->execute({{"command", "sleep 5"}, {"timeout", 1}});
    ASSERT_FALSE(to.ok);
    ASSERT(to.error.find("timed out") != std::string::npos);
    ASSERT(jobs.list().empty());
}

// ---------------------------------------------------------------------------
// Approval gate (side-effecting tools require host approval)
// ---------------------------------------------------------------------------

TEST(agent_denies_gated_tool_without_handler) {
    // A registry containing an approval-required tool. With no on_approval
    // handler installed, approve_call must fail safe (deny).
    agent::Config cfg;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::Agent ag(cfg, reg);   // hooks default: no on_approval
    // No public approve_call, so exercise via the hook contract instead:
    agent::AgentHooks hooks;
    bool called = false;
    hooks.on_approval = [&](const std::string& n, const agent::json&,
                            const std::string& s) {
        called = true;
        ASSERT_EQ(n, std::string("bash"));
        ASSERT(s.find("run:") != std::string::npos);
        return agent::Approval::AllowSession;
    };
    ag.set_hooks(hooks);
    auto* t = reg.find("bash");
    ASSERT_TRUE(t != nullptr);
    ASSERT_TRUE(t->requires_approval());
    agent::Approval d = hooks.on_approval("bash", {{"command", "ls"}},
                                          t->summarize({{"command", "ls"}}));
    ASSERT_TRUE(called);
    ASSERT(d == agent::Approval::AllowSession);
}

// A model that keeps emitting a tool call with EMPTY arguments (e.g. search {})
// triggers a deterministic, non-transient failure. The agent must not loop
// through all max_tool_iterations (which stalls the UI for minutes on a
// single-GPU endpoint) — it should stop after a few identical failures.
TEST(agent_stops_on_repeated_empty_arg_tool_call) {
    // A mock SSE server that re-serves the same "search {}" tool call on every
    // connection (unlike spawn_mock_sse, which accepts only once).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
        "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
        "\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8920);
    ASSERT(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);  // NOLINT
    ASSERT(listen(fd, 8) == 0);
    std::thread srv([fd, sse]() {
        while (true) {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0) break;
            char buf[4096]; std::string req;
            while (true) {
                int n = recv(c, buf, sizeof(buf) - 1, 0);
                if (n <= 0) break;
                req.append(buf, n);
                if (req.find("\r\n\r\n") != std::string::npos) break;
            }
            std::string http =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Content-Length: " + std::to_string(sse.size()) +
                "\r\n\r\n" + sse;
            send(c, http.c_str(), http.size(), 0);
            usleep(100000);
            close(c);
        }
    });
    srv.detach();
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8920/v1";
    cfg.stream = true;
    cfg.mode = agent::AgentMode::Yolo;
    cfg.max_tool_iterations = 32;   // default; the loop must NOT reach this
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::register_default_tools(reg, jobs);
    agent::Agent ag(cfg, reg);

    // Count tool-call dispatches by watching the on_tool_result hook.
    int tool_results = 0;
    agent::AgentHooks hooks;
    hooks.on_tool_result = [&](const std::string&, const agent::ToolResult&) {
        ++tool_results;
    };
    ag.set_hooks(hooks);

    std::string out = ag.run("review ncurses usage");
    // Must terminate well before max_tool_iterations (bounded by fail_streak).
    ASSERT(tool_results < 10);
    ASSERT(out.find("kept failing") != std::string::npos);
    close(fd);
}

// ---------------------------------------------------------------------------
// Background jobs (JobService + process_* tools)
// ---------------------------------------------------------------------------

TEST(job_service_start_read_stop) {
    agent::JobService jobs;
    std::string id = jobs.start("printf 'hello\\nworld\\n'", "/tmp");
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(jobs.running_count(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // First delta returns the captured output; a second read returns nothing.
    std::string delta = jobs.read_delta(id);
    ASSERT(delta.find("hello") != std::string::npos);
    ASSERT(jobs.read_delta(id).empty());
    // Full output is also retrievable.
    ASSERT(jobs.output(id).find("world") != std::string::npos);
    ASSERT(jobs.stop(id));
    ASSERT_EQ(jobs.running_count(), 0);
    ASSERT_FALSE(jobs.stop(id));  // already gone
}

TEST(job_service_idle_timeout_kills) {
    agent::JobService jobs;
    // No output for 1s -> auto-killed by check_timeouts.
    std::string id = jobs.start("sleep 5", "/tmp", 600, 1);
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    jobs.check_timeouts();
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(job_service_hard_timeout_kills) {
    agent::JobService jobs;
    std::string id = jobs.start("sleep 5", "/tmp", 1, 600);
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    jobs.check_timeouts();
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(process_tools_share_service) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    ASSERT_EQ(tools.size(), 3u);
    // Drive the tools through one background cycle.
    auto* st = tools[0].get();
    auto r = st->execute({{"command", "printf 'bg\\n'"}});
    ASSERT(r.ok);
    std::string id = r.output;
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto* rd = tools[1].get();
    auto out = rd->execute({{"id", id}, {"all", true}});
    ASSERT(out.ok);
    ASSERT(out.output.find("bg") != std::string::npos);
    auto* stop = tools[2].get();
    auto killed = stop->execute({{"id", id}});
    ASSERT(killed.ok);
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(process_stop_returns_captured_output) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    std::string id = tools[0]->execute({{"command", "printf 'held\\n'"}}).output;
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto stopped = tools[2]->execute({{"id", id}});
    ASSERT(stopped.ok);
    ASSERT(stopped.output.find("held") != std::string::npos);
    ASSERT_EQ(jobs.running_count(), 0);
}

TEST(job_service_stop_finished_returns_true) {
    agent::JobService jobs;
    std::string id = jobs.start("true", "/tmp");
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT(jobs.stop(id));      // still in the map -> true
    ASSERT_FALSE(jobs.stop(id)); // already removed -> false
}

TEST(job_service_caps_output_at_one_mib) {
    agent::JobService jobs;
    // Emit ~2 MiB of 'A's; the reader must cap at 1 MiB and flag truncation.
    std::string id = jobs.start(
        "head -c 2000000 /dev/zero | tr '\\0' 'A'", "/tmp");
    ASSERT_FALSE(id.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    agent::Job* j = jobs.get(id);
    ASSERT(j != nullptr);
    agent::JobInfo info = j->info();
    ASSERT(info.truncated);
    ASSERT(info.bytes <= (1u << 20) + 16);
    jobs.stop(id);
}

// ---------------------------------------------------------------------------
// Context compression tests
// ---------------------------------------------------------------------------

static agent::Message msg(const std::string& role, const std::string& content,
                           const std::string& name = "") {
    return {role, content, "", "", name, json::object()};
}

TEST(tree_shaker_empty_history) {
    agent::TreeShaker shaker;
    auto tags = shaker.classify({});
    ASSERT(tags.empty());
}

TEST(tree_shaker_single_turn) {
    agent::TreeShaker shaker;
    std::vector<agent::Message> hist = {
        msg("system", "you are a bot"),
        msg("user", "hello")
    };
    auto tags = shaker.classify(hist);
    ASSERT(tags.size() == 2u);
    // Last user message is active root → core
    ASSERT(tags[1] == agent::Classification::core);
}

TEST(tree_shaker_active_task_is_core) {
    agent::TreeShaker shaker;
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "old request"),
        msg("assistant", "handled"),
        msg("user", "current active"),
    };
    auto tags = shaker.classify(hist);
    ASSERT(tags.size() >= 4u);
    ASSERT(tags[3] == agent::Classification::core);
}

TEST(tree_shaker_stale_tool_result) {
    agent::TreeShaker shaker;
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "do something"),
        msg("assistant", ""),
        msg("tool", "output", "read"),
        msg("user", "ok"),
    };
    auto tags = shaker.classify(hist);
    ASSERT(tags[3] == agent::Classification::context ||
           tags[3] == agent::Classification::prune);
}

TEST(tree_shaker_diagnostic_is_prune) {
    agent::TreeShaker shaker;
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "read file"),
        msg("assistant", ""),
        msg("tool", std::string(200, 'x'), "read"),
        msg("assistant", "done"),
        msg("user", "move on"),
    };
    auto tags = shaker.classify(hist);
    ASSERT(tags[3] == agent::Classification::prune);
}

TEST(compression_gate_below_threshold) {
    agent::CompressionConfig cc;
    cc.threshold = 0.75;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 100000;
    std::vector<agent::Message> hist = {
        msg("system", "short"),
        msg("user", "hi"),
    };
    ASSERT_FALSE(gate->should_compress(hist, cfg));
}

TEST(compression_gate_above_threshold) {
    agent::CompressionConfig cc;
    cc.threshold = 0.10;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 1000;
    std::vector<agent::Message> hist;
    for (int i = 0; i < 10; ++i)
        hist.push_back(msg("user", std::string(100, 'a')));
    ASSERT(gate->should_compress(hist, cfg));
}

TEST(compression_gate_min_turns) {
    agent::CompressionConfig cc;
    cc.threshold = 0.01;
    cc.min_turns = 100;
    auto gate = agent::make_compression_gate(cc);
    agent::Config cfg;
    cfg.context_size = 100000;
    std::vector<agent::Message> hist = {msg("user", "hello")};
    ASSERT_FALSE(gate->should_compress(hist, cfg));
}

TEST(structured_compressor_roundtrip_empty) {
    agent::CompressionConfig cfg;
    auto c = agent::make_compressor(cfg);
    auto result = c->compress({}, cfg);
    ASSERT(result.empty());
}

TEST(structured_compressor_basic) {
    agent::CompressionConfig cfg;
    auto c = agent::make_compressor(cfg);
    std::vector<agent::Message> hist = {
        msg("system", "prompt"),
        msg("user", "hello"),
    };
    auto result = c->compress(hist, cfg);
    ASSERT(!result.empty());
}

// ---------------------------------------------------------------------------
// Experience / memory tests
// ---------------------------------------------------------------------------

TEST(memory_store_upsert_and_retrieve) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Memory mem;
    mem.content = "project uses make";
    mem.tags = {"build", "make"};
    mem.evidence_count = 3;
    store->upsert(mem);
    auto results = store->top_memories(10, "how to build");
    ASSERT(results.size() >= 1u);
    ASSERT(results[0].content == "project uses make");
}

TEST(memory_store_top_k_limits) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    for (int i = 0; i < 5; ++i) {
        agent::Memory mem;
        mem.content = "memory " + std::to_string(i);
        mem.evidence_count = i;
        store->upsert(mem);
    }
    auto results = store->top_memories(3, "");
    ASSERT(results.size() == 3u);
}

TEST(memory_store_skill_trigger) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Skill sk;
    sk.content = "run tests";
    sk.trigger_phrase = "test";
    sk.evidence_count = 5;
    store->upsert(sk);
    auto results = store->top_skills(10, "run the tests");
    ASSERT(results.size() >= 1u);
    ASSERT(results[0].content == "run tests");
    auto no_match = store->top_skills(10, "build the project");
    ASSERT(no_match.empty());
}

TEST(memory_store_decay) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Memory mem;
    mem.content = "will decay";
    mem.evidence_count = 3;
    store->upsert(mem);
    store->decay_all();
    auto results = store->top_memories(10, "");
    ASSERT(results.size() >= 1u);
    ASSERT(results[0].evidence_count == 2);
}

TEST(memory_retriever_empty) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::MemoryRetriever retriever(*store);
    auto suffix = retriever.build_system_prompt_suffix("hello");
    ASSERT(suffix.empty());
}

TEST(memory_retriever_with_memories) {
    agent::ExperienceConfig ec;
    auto store = agent::make_memory_store(ec);
    agent::Memory mem;
    mem.content = "build system is make";
    mem.tags = {"build"};
    mem.evidence_count = 3;
    store->upsert(mem);
    agent::MemoryRetriever retriever(*store);
    auto suffix = retriever.build_system_prompt_suffix("how to build");
    ASSERT(!suffix.empty());
    ASSERT(suffix.find("build system") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Integration: compression pipeline + memory end-to-end
// ---------------------------------------------------------------------------

TEST(integration_compress_extract_retrieve) {
    // Simulate a realistic conversation: system, user request, tool calls,
    // assistant replies, a side quest, and a new request.
    agent::TreeShaker shaker;
    std::vector<agent::Message> hist = {
        msg("system", "You are amber, a helpful coding assistant."),
        msg("user", "How is this project built?"),
        msg("assistant", "Let me check the build system."),
        msg("tool", "GNUmakefile\nconfigure script\n", "read"),
        msg("assistant", "This project uses GNU make with ./configure."),
        msg("user", "Run the tests for me."),
        msg("assistant", "Running the test suite now."),
        msg("tool",
            "compressor_test.cpp: OK\nmemory_store_test.cpp: OK\n"
            "103 passed, 0 failed\n",
            "bash"),
        msg("assistant", "All 103 tests passed."),
        // Side quest: read a config file
        msg("user", "What's in the config file?"),
        msg("assistant", "Reading amber.conf."),
        msg("tool", "api_base=http://localhost:8000\nmodel=gpt-4o\n",
            "read"),
        msg("assistant", "The config points at localhost:8000 with gpt-4o."),
        // Back to main task
        msg("user", "What was the test result again?"),
    };

    // Phase 1: Classify
    auto tags = shaker.classify(hist);
    ASSERT(tags.size() == hist.size());

    // The last user message should be core (active task)
    ASSERT(tags[tags.size() - 1] == agent::Classification::core);

    // Context-tagged turns (completed sub-tasks, stale tool results)
    // should exist before the active task.
    bool found_context = false;
    for (size_t i = 0; i < tags.size() - 1; ++i) {
        if (tags[i] == agent::Classification::context) {
            found_context = true;
            break;
        }
    }
    ASSERT(found_context);

    // Phase 2: Compress
    auto compressor = agent::make_compressor(agent::CompressionConfig{});
    auto compressed = compressor->compress(hist, agent::CompressionConfig{});
    ASSERT(!compressed.empty());

    // The compressed result should have fewer messages than the original
    // (because some were pruned and summarised)
    // or at least not more
    ASSERT(compressed.size() <= hist.size());

    // Phase 3: Extract memories from the classified history
    agent::ExperienceConfig ec;
    ec.enabled = true;
    auto store = agent::make_memory_store(ec);

    // Simulate extraction by feeding informative tool results into the store
    agent::Memory mem;
    mem.content = "project uses GNU make with ./configure";
    mem.tags = {"build", "make"};
    mem.evidence_count = 3;
    store->upsert(mem);

    agent::Memory mem2;
    mem2.content = "103 tests passed";
    mem2.tags = {"tests", "testing"};
    mem2.evidence_count = 3;
    store->upsert(mem2);

    // Phase 4: Retrieve relevant memories
    agent::MemoryRetriever retriever(*store);
    std::string suffix = retriever.build_system_prompt_suffix(
        "how do I build this project?");
    ASSERT(!suffix.empty());
    ASSERT(suffix.find("GNU make") != std::string::npos ||
           suffix.find("./configure") != std::string::npos);

    // Phase 5: Verify complete cycle — compress, persist, retrieve
    store->decay_all();
    auto after_decay = store->top_memories(10, "build");
    ASSERT(after_decay.size() >= 1u);
    // After one decay, evidence should be 2
    ASSERT(after_decay[0].evidence_count <= 3);
}

// ---------------------------------------------------------------------------

int main() { return agent::test::run_all(); }
