## Spec: ANSI Parsing (SGR → Run Styles)

### Purpose
Parse ANSI SGR (Select Graphic Rendition) escape sequences embedded in LLM
output and map them to `rich::Run` style flags (bold, dim, italic, underline)
and colour-pair indices. Also strip ANSI from text for plain-text copy.

### Ownership
- **Source files**: `tui/markdown_md4c.cpp` (`append_styled()` — lines 200–280), `tui/textutil.cpp` (ANSI stripping — `text::strip_ansi()`), `tui/rich.h` (`Run` style flags)
- **Test files**: `tests/tui_tests.cpp` — `markdown_maps_inline_ansi_sgr_to_runs` (line 118)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | UTF-8 text string possibly containing ANSI SGR escape sequences (`\033[<N>m`) |
| **Output** | `std::vector<rich::Run>` with parsed styles. SGR codes stripped from output text. |
| **Error states** | Unknown/unsupported SGR codes → skipped (left at current style). Malformed sequences → best-effort parse. |
| **Invariants** | See below. |
| **Thread safety** | Stateless — safe to call from any thread. |

### Invariants

1. SGR codes `\033[0m` reset all styles to default (base pair, no bold/dim/italic/underline).
2. SGR codes 1–9 map to visual styles (bold, dim, italic, underline, blink, inverse).
3. SGR codes 30–37 and 90–97 (foreground colours) map to palette colour pairs via a lookup table.
4. SGR codes 38 (`:5:` 256-colour, `:2:` truecolour) and 48 (background) are NOT supported.
5. Unrecognised CSI sequences (cursor movement, erase, etc.) are NOT parsed but passed through.
6. Each change in style produces a new `rich::Run` — adjacent runs with identical style are merged later.

---

### Scenarios

#### [AN-01] Basic SGR reset/dim/bold/italic

- **Given**: A string with SGR formatting codes
- **Input**: `"\033[1mbold\033[0m \033[3mitalic\033[0m \033[2mdim\033[0m"`
- **Expected**: Three runs: `"bold"` with `bold=true`, `"italic"` with `italic=true`, `"dim"` with `dim=true`. Separating space and final reset produce runs with default style.
- **On failure**: SGR codes visible in output text.

#### [AN-02] SGR foreground colour 31 (red)

- **Given**: Red text
- **Input**: `"\033[31mred text\033[0m"`
- **Expected**: One run with `pair` mapped to the red colour pair. No `\033[31m` in output text.

#### [AN-03] SGR foreground colour 92 (bright green)

- **Given**: Bright green text
- **Input**: `"\033[92mgreen\033[0m"`
- **Expected**: Bright green colour pair applied (90–97 range maps to bright colours).
- **On failure**: Green maps to default or wrong pair.

#### [AN-04] Combined: bold + colour

- **Given**: Bold red text
- **Input**: `"\033[1;31mbold red\033[0m"`
- **Expected**: Single run with `bold=true` AND the red colour pair. Both attributes applied.
- **On failure**: Only one attribute applied.

#### [AN-05] 256-colour SGR (unsupported)

- **Given**: `38;5;N` sequence
- **Input**: `"\033[38;5;196mtext\033[0m"`
- **Expected**: Not parsed. The `38;5;196` parameters exceed the 2-digit parser limit. Original style retained. Sequence characters are consumed but no colour change.
- **Known limit**: Only 1–2 digit parameters supported.

#### [AN-06] Truecolour SGR (unsupported)

- **Given**: `38;2;R;G;B` sequence
- **Input**: `"\033[38;2;255;0;0mtext\033[0m"`
- **Expected**: Not parsed. Same as 256-colour — exceeds parser limit. Original style retained.

#### [AN-07] Non-SGR ANSI sequences passed through

- **Given**: Cursor movement or erase sequence
- **Input**: `"\033[2J\033[H"`
- **Expected**: Not parsed by `append_styled()`. These sequences pass through to output text. The LLM is expected not to send them, but if they arrive, they are rendered verbatim (escape codes visible as mojibake in ncurses).

#### [AN-08] No ANSI — passthrough

- **Given**: Plain text with no escape sequences
- **Input**: `"Hello world"`
- **Expected**: Single `rich::Run` with default style. No splitting.
- **On failure**: Unnecessary runs created.

#### [AN-09] Stripping ANSI for plain-text copy

- **Given**: Text with ANSI codes
- **Input**: `strip_ansi("\033[32mOK\033[0m")`
- **Expected**: Returns `"OK"`. All SGR codes stripped.
- **On failure**: ANSI codes remain in plain text output.

#### [AN-10] Reset in middle of text

- **Given**: Bold text, reset, then normal
- **Input**: `"\033[1mBold\033[0m normal"`
- **Expected**: Two runs: `"Bold"` with `bold=true`, then `" normal"` with default style.

---

### Cross-references

- **Depends on**: `display/markdown-parser.md` (called from `text_cb` in markdown pipeline)
- **Depended on by**: `display/table-rendering.md` (ANSI in table cells — known gap), `docs/spec/INDEX.md`
- **Test coverage**: `tests/tui_tests.cpp`: `markdown_maps_inline_ansi_sgr_to_runs` (line 118)

### Known gaps

1. **256-colour (`38;5;N`) and truecolour (`38;2;R;G;B`) not supported** — Only 16-colour SGR (0–107) parsed. Extended colour sequences are silently consumed but ignored.
2. **Background colours not supported** — SGR 48 (background) and all background colour variants are not parsed.
3. **Non-SGR CSI sequences passed through** — Cursor positioning, erase, and other escape codes arrive in output text as raw escape bytes.
4. **ANSI in table cells** — `append_styled()` is NOT called for table cell content. ANSI codes in cells arrive as raw bytes. (TR-09 known gap.)
