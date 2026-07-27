## Spec: Table Rendering

### Purpose
Render markdown tables (`|...|` syntax) as box-drawing rich text in the TUI
scrollback. The LLM frequently emits tables in its responses; they must be
legible, properly aligned, and gracefully handle the LLM's habit of emitting
non-standard markdown (missing delimiters, missing blank lines, ragged columns).

### Ownership
- **Source files**: `tui/markdown_md4c.cpp`, `tui/markdown.h`, `tui/rich.h`, `tui/rich.cpp`, `tui/canvas.cpp`
- **Test files**: `tests/tui_tests.cpp` (currently 3 test blocks)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Well-formed markdown table per GFM spec, OR the subset the LLM emits (see Scenarios) |
| **Output** | `std::vector<rich::Line>` with box-drawing border characters and cell content |
| **Error states** | Invalid/malformed tables render as plain text (graceful degradation) |
| **Invariants** | See below |
| **Thread safety** | Called synchronously from `append_markdown()` on agent-worker thread; output consumed on main render thread |

### Invariants

1. Every table row has exactly the same number of visual columns.
2. Cell content is padded/shown to the column width; no visual bleeding into adjacent columns.
3. The header row is visually distinct from body rows (bold or colour via `P_MD_HEAD`).
4. A separator line (`├──┼──┤`) exists between header and body.
5. Column widths are consistent across all rows — not per-row computed.
6. No UTF-8 box-drawing characters leak into plain-text-copied output.
7. ANSI escape sequences inside cell text are parsed and rendered as styled runs (not raw `\x1b[...` bytes).
8. Empty cells render as empty columns with borders intact.
9. A blank line follows the table to separate from subsequent content.

---

### Scenarios

#### [TR-01] Standard three-column table

- **Given**: a markdown table with header, delimiter, and 2+ body rows
- **Input**:
  ```
  | Name | Age | City |
  |------|-----|------|
  | Alice | 30 | New York |
  | Bob | 25 | London |
  ```
- **Expected**: 5 output lines: header row, separator line, 2 body rows, blank line. Each column width matches the widest cell across ALL rows.
- **On failure**: Table should render as plain text line-by-line (no box chars).

#### [TR-02] Delimiter-only table (no header content)

- **Input**: `|---|---|---|`
- **Expected**: Single separator line `├──┼──┼──┤`.
- **Rationale**: Some LLMs emit delimiter-only lines.

#### [TR-03] Missing delimiter row (LLM regression)

- **Given**: A table with header rows but no `|---|---|` separator
- **Input**:
  ```
  | Priority | Count |
  | High     | 12    |
  | Low      | 5     |
  ```
- **Expected**: `normalize_markdown()` synthesises a `|---|---|` row after the first row. Output is identical to [TR-01].
- **Regression marker**: `markdown_repairs_table_missing_delimiter_row` test.

#### [TR-04] Missing blank line before table

- **Given**: Table immediately follows prose with no blank line (LLM common error)
- **Input**:
  ```
  Summary of priorities
  | Priority | Count |
  |---|---|
  | High | 12 |
  ```
- **Expected**: `normalize_markdown()` inserts a blank line before the table. md4c does not eat the preceding text.
- **Regression marker**: `markdown_renders_table_without_leading_blank_line` test.

#### [TR-05] Column alignment (center/right)

- **Given**: A table with alignment markers in the delimiter
- **Input**:
  ```
  | Left | Center | Right |
  |:-----|:------:|------:|
  | a    | b      | c     |
  ```
- **Expected**: Content is left/center/right-aligned within each column, determined by the delimiter format (`:---` left, `:---:` center, `---:` right).
- **On failure**: Default to left-aligned. This is a KNOWN GAP — currently alignment is parsed but not rendered.

#### [TR-06] Column width consistency across rows

- **Given**: A table where the header has narrower content than a body row (e.g. header `| X |` body `| LongText |`)
- **Input**:
  ```
  | X |
  |---|
  | LongText |
  ```
- **Expected**: All rows use the same per-column width: `max(header_width, body_width)` for each column.
- **Regression note**: Currently width is per-row. If the separator line is too narrow to cover the body cell, the pipe alignment breaks.

#### [TR-07] Cell with multi-byte / emoji characters

- **Given**: A table cell containing CJK characters or emoji
- **Input**:
  ```
  | Icon | Description |
  |------|-------------|
  | 🔥   | Fire        |
  | 中国 | Country     |
  ```
- **Expected**: Column widths respect `wcwidth()` — double-width characters occupy 2 columns. Cells align correctly, no overflow.
- **On failure**: No phantom truncation; `wcwidth` return `-1` handled as width 1 (replacement char).

#### [TR-08] Empty cells

- **Given**: A table with empty cells
- **Input**:
  ```
  | A | B | C |
  |---|---|---|
  | 1 |   | 3 |
  ```
- **Expected**: Empty cell renders as `│   │` (spaces only). Column widths account for max content across all rows, so empty cells don't shrink the column.

#### [TR-09] ANSI escapes inside table cells

- **Given**: LLM returns ANSI-coloured text inside a table cell
- **Input**:
  ```
  | File | Status |
  |------|--------|
  | foo  | \x1b[32mOK\x1b[0m |
  ```
- **Expected**: The ANSI codes are parsed into rich::Run styles. "OK" renders in green. Raw `\x1b[` bytes do not appear.
- **Regression note**: CURRENTLY BROKEN — `text_cb` accumulates cell buffer verbatim without calling `append_styled()`.

#### [TR-10] No ASCII fallback

- **Given**: Terminal does not support UTF-8 (`AMBER_ASCII=1`)
- **Expected**: Table borders fall back to ASCII characters: `|` for vertical, `-` for horizontal, `+` for joints.
- **On failure**: This is a KNOWN GAP — table borders are hardcoded UTF-8 box-drawing characters with no ASCII fallback.

#### [TR-11] Single-cell table

- **Given**: A one-column, one-row table
- **Input**:
  ```
  | lone |
  |------|
  | cell |
  ```
- **Expected**: Renders as `│ lone │`, separator `├─────┤`, `│ cell │`. Works like a multi-column table with `ncol=1`.

#### [TR-12] Table embedded in list or blockquote

- **Given**: A table inside a blockquote or between list items
- **Input**:
  ```
  > | A | B |
  > |---|---|
  > | 1 | 2 |
  ```
- **Expected**: The table renders with the blockquote prefix (`>` typically stripped by md4c, feature handled by the markdown pipeline; ensure no double-prefixing).

---

### Cross-references

- **Depends on**: `display/markdown-parser.md`, `display/ansi-parsing.md`, `tui/layout-engine.md` (for canvas rendering and scroll)
- **Depended on by**: `docs/spec/INDEX.md` (TUI display category)
- **Test coverage**:
  - `tests/tui_tests.cpp`: `markdown_renders_aligned_table_and_skips_divider`,
    `markdown_renders_table_without_leading_blank_line`,
    `markdown_repairs_table_missing_delimiter_row`

### Known gaps (documented tech debt)

1. **Per-row column widths** — width is computed per-row, not globally. Should track max width per column across all rows in the table.
2. **Alignment not rendered** — `MD_ALIGN_*` is captured but ignored. Center/right alignment has no visual effect.
3. **ANSI escapes in cells** — `append_styled()` is not called in cell text accumulation path.
4. **No ASCII fallback** — Box-drawing chars are hardcoded UTF-8; `AMBER_ASCII=1` has no effect on tables.
5. **No per-column word-wrapping** — Long cell text is word-wrapped at the terminal edge, not at the column boundary. Pipes may visually misalign.
6. **No outer table border** — Top (`┌──┬──┐`) and bottom (`└──┴──┘`) borders are not rendered.
