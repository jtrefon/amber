## Spec: Markdown Parser (md4c Pipeline)

### Purpose
Convert LLM-emitted markdown into styled `rich::Line` vectors for TUI scrollback
rendering. The LLM frequently emits non-standard markdown — the pipeline must
normalise these imperfections before the md4c parser sees them, then map every
markdown element (headings, lists, code, tables, blockquotes, inline formatting)
into styled runs that the canvas can paint.

### Ownership
- **Source files**: `tui/markdown_md4c.cpp` (717 lines), `tui/markdown.h`, `tui/markdown.cpp` (`highlight()`), `tui/rich.h`, `tui/rich.cpp` (`wrap()`)
- **Test files**: `tests/tui_tests.cpp` (16 test blocks, lines 102–308)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Raw markdown string from LLM. May be malformed (missing blank lines, missing table delimiters, embedded rule chars). |
| **Output** | `std::vector<rich::Line>`: ordered display lines, each with styled `Run[]` vectors. A `rich::Line` has flags: `is_code`, `is_hr`, `heading_level`. |
| **Error states** | Unparseable markdown renders as plain text lines. md4c parse abort → empty output. No exceptions propagate past `render()`. |
| **Invariants** | See below. |
| **Thread safety** | Called from agent-worker thread via `Tui::append_markdown()`; output consumed on render thread. No shared mutable state. |

### Invariants

1. Every markdown element produces at least one `rich::Line`. Empty inputs produce `{}` (single empty line).
2. Inline formatting (bold, italic, code, links, strikethrough) is reflected in `Run::bold`, `Run::italic`, etc., not in the text string.
3. ANSI SGR escape sequences in the input are parsed and applied to `Run` styles, not emitted as raw bytes.
4. Fenced code blocks produce one `rich::Line` per source line, each with `is_code = true`.
5. Headings produce lines formatted with `P_MD_HEAD` color pair and `bold = true`.
6. Horizontal rules produce a single line with `is_hr = true`.
7. Nested blockquotes produce increasingly indented `> ` prefixes.
8. Ordered lists produce sequential numbering (`1.`, `2.`, `3.`).
9. The `normalize_markdown()` preprocessor handles the 3 known LLM emission defects: missing blank line before table, missing delimiter row, and embedded separator runs.

---

### Scenarios

#### [MD-01] Plain paragraph text

- **Given**: A string with no markdown syntax
- **Input**: `"Hello world"`
- **Expected**: Single `rich::Line` with one `rich::Run` containing `"Hello world"`, default text pair.
- **On failure**: Empty output or dropped text.

#### [MD-02] Headings (levels 1–6)

- **Given**: A heading line
- **Input**: `"## Section Title"`, `"# Title"`
- **Expected**: Heading produces a `rich::Line` with `bold = true` runs, `P_MD_HEAD` color. Trailing whitespace after the `#` markers is stripped.
- **Regression guard**: `markdown_trims_heading_whitespace`, `markdown_bare_hash_markers_do_not_crash`.

#### [MD-03] Bare hash markers (edge case)

- **Given**: Input consisting only of markdown prefixes with no content
- **Input**: `"#"`, `"###"`, `">"`, `"-"`
- **Expected**: No crash. Renders as empty heading, empty blockquote, or empty list item (visual blank line).
- **Regression guard**: `markdown_bare_hash_markers_do_not_crash`.

#### [MD-04] Inline formatting

- **Given**: Text with bold, italic, code, strikethrough, and links
- **Input**: `"**bold** *italic* \`code\` ~~strikethrough~~"`
- **Expected**: Each span produces a `Run` with the corresponding style flag. Bold → `bold = true`. Italic → `italic = true`. Code → `pair = code_pair`. Strikethrough → `dim = true`. Link → `pair = link_pair, under = true`.
- **On failure**: Run flags not set, or formatting characters (`**`, `*`, `` ` ``) visible in output.

#### [MD-05] Inline code with special characters

- **Given**: Backtick-enclosed inline code containing markdown characters
- **Input**: `` "Use `std::vector<int>` for the list" ``
- **Expected**: The span between backticks renders as a single run with `pair = code_pair`. The `*`, `::`, `<>` inside are not interpreted as markdown.

#### [MD-06] ANSI escape sequences in text

- **Given**: Text containing raw ANSI SGR codes
- **Input**: `"Status: \033[32mOK\033[0m"`
- **Expected**: `append_styled()` parses `\033[32m` → green foreground, `\033[0m` → reset. Output has two runs: one normal, one with green color pair. No raw `\x1b[` bytes visible.
- **Regression guard**: `markdown_maps_inline_ansi_sgr_to_runs`.
- **Limitation**: Only SGR codes 0–107 with ≤2 numeric parameters are supported. 256-color (`38;5;N`) and truecolor (`38;2;R;G;B`) are NOT parsed.

#### [MD-07] Fenced code block with language

- **Given**: A triple-backtick fenced code block
- **Input**:
  ````
  ```cpp
  int main() { return 0; }
  ```
  ````
- **Expected**: `enter_block(MD_BLOCK_CODE)` captures `lang = "cpp"`. Each source line becomes a `rich::Line` with `is_code = true`. Lexer tokenizes: keywords bolded, strings in `P_MD_CODESTR`, numbers in `P_MD_CODENUM`, comments in `P_MD_CODECMT`.
- **Known gap**: The `lang` parameter is NOT passed to the highlighter — same keyword list for all languages.
- **Regression guard**: `markdown_highlight_colors_fenced_code`.

#### [MD-08] Ordered list numbering

- **Given**: A numbered list
- **Input**:
  ```
  1. First
  2. Second
  3. Third
  ```
- **Expected**: Each item prefixed with sequential number (`1.`, `2.`, `3.`) regardless of source numbers. md4c provides `d->start` for the starting index.
- **Regression guard**: `markdown_ordered_list_numbers_sequentially`.

#### [MD-09] Nested unordered lists

- **Given**: A nested bullet list
- **Input**:
  ```
  - Outer
    - Inner
      - Deepest
  ```
- **Expected**: Each nesting level adds 2-space indentation. Each item prefixed with `•`. Inner items correctly indented under parent text.
- **Regression guard**: `markdown_nested_list_items_separate`.

#### [MD-10] Task list items

- **Given**: A task list with checked and unchecked items
- **Input**:
  ```
  - [x] Done
  - [ ] Todo
  ```
- **Expected**: `[x]` and `[ ]` text is rendered verbatim as part of the list item content. md4c detects `is_task` but the renderer does NOT substitute checkbox glyphs.
- **Known gap**: Task list checkboxes are not styled differently. No glyph substitution for checked/unchecked.

#### [MD-11] Blockquote — multiple lines

- **Given**: A multi-line blockquote
- **Input**:
  ```
  > Line one
  > Line two
  ```
- **Expected**: Each line rendered independently with `> ` prefix per line. Soft breaks inside blockquotes flush the current line so each source line gets its own quoted visual line.
- **Regression guard**: `markdown_blockquote_each_line_quoted`.

#### [MD-12] Blockquote — nested

- **Given**: Nested blockquotes
- **Input**:
  ```
  > Outer
  > > Inner
  ```
- **Expected**: Inner quotes produce `> > ` prefix with additional indentation. `quote_depth` increases per nesting level.

#### [MD-13] Horizontal rule (md4c path)

- **Given**: A markdown horizontal rule
- **Input**:
  ```
  ---
  ```
- **Expected**: `leave_block(MD_BLOCK_HR)` emits a `rich::Line` with `is_hr = true`. Canvas renders `ACS_HLINE` across full terminal width using `hr_pair` color.

#### [MD-14] Horizontal rule (fallback path — embedded separator in prose)

- **Given**: A line of separator characters embedded in text, not on its own line
- **Input**: `` "Something\n─────────────────────\n" ``
- **Expected**: `flush_block()` calls `is_separator_line()` which detects 12+ consecutive rule characters. The line is emitted as a horizontal rule instead of text. Separator runs are split via `normalize_markdown()` before md4c.
- **Regression guard**: `markdown_splits_embedded_separator_rule`.

#### [MD-15] Code block with empty content

- **Given**: A fenced code block with no content
- **Input**:
  ````
  ```lang
  ```
  ````
- **Expected**: `leave_block(MD_BLOCK_CODE)` emits a single blank `rich::Line{}`. No crash.

#### [MD-16] Mixed inline code and ANSI

- **Given**: A paragraph containing both inline code and ANSI escapes
- **Input**: `` "Run \033[1m`deploy.sh`\033[0m to start" ``
- **Expected**: Inline code span gets `code_pair` coloring. Bold ANSI around it gets `bold = true`. The two formatting mechanisms compose correctly.

#### [MD-17] Long line word-wrapping

- **Given**: A paragraph that exceeds terminal width
- **Input**: `"A very long line that exceeds the canvas display width by a significant margin ..."`
- **Expected**: `rich::wrap()` splits into multiple physical lines at word boundaries. Each wrapped segment preserves the parent run's style (bold, italic, color). Multi-byte characters are not split mid-glyph.

#### [MD-18] Error recovery — parse failure

- **Given**: md4c returns non-zero (internal error)
- **Input**: Deeply nested or malformed markdown
- **Expected**: `render()` returns empty `vector<Line>`. No exception, no crash. `normalize_markdown()` catches most defects before md4c.

#### [MD-19] Empty input

- **Given**: Empty string
- **Input**: `""`
- **Expected**: `vector<Line>{Line{}}` (single empty line). No crash, no infinite loop.

---

### Cross-references

- **Depends on**: `display/table-rendering.md`, `display/ansi-parsing.md`, `display/icon-rendering.md`
- **Depended on by**: `tui/event-loop.md`, `docs/spec/INDEX.md` (display category)
- **Test coverage**: `tests/tui_tests.cpp` (lines 102–308): headings, inline formatting, ANSI, fenced code, tables (3 tests), separator rules, blockquotes, lists, task lists, edge cases.

### Known gaps

1. **`Style::emph_pair` is dead code** — declared in `Style` struct but never read. Bold/italic always use the current pair.
2. **`highlight()` ignores `lang`** — Fenced code blocks receive the same keyword list regardless of language. C++, Python, shell all highlight identically.
3. **Task lists have no visual checkbox** — `[x]` / `[ ]` rendered as literal text. No glyph substitution or color change.
4. **Inline images** — `MD_SPAN_IMG` gets link color but no special rendering. No image viewer integration.
5. **Strikethrough uses `dim`** — ncurses has no native strike-through. Implemented as `dim = true` as best-effort.
6. **Limited SGR support** — Only 0–107 codes parsed. `38;5;N` (256-color), `38;2;R;G;B` (truecolor), and non-SGR CSI sequences are not handled.
7. **No HTML passthrough** — `MD_FLAG_NOHTML` not set; inline HTML is passed through verbatim by md4c.
8. **No nesting depth limit** — `block_prefix()` loops over all list/quote levels without bound. Deeply nested input could produce very long prefix strings.
