## Spec: Icon Rendering (Nerd Font / ASCII Fallback)

### Purpose
Map UI glyphs (box-drawing characters, status icons, bullet points) to either
Unicode Nerd Font glyphs or ASCII fallbacks based on the `AMBER_ASCII` env var.
Also ensure double-width characters (emoji, CJK) are correctly measured and
rendered without breaking the layout.

### Ownership
- **Source files**: `tui/glyphs.h` (glyph set definitions), `tui/textutil.h`/`.cpp` (`text::display_cols()`, `text::to_wide()`, `text::glyph()`)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `AMBER_ASCII` env var (set = use ASCII fallback, unset = use Unicode) |
| **Output** | `text::glyph(Set::Type)` returns the appropriate glyph string for the current mode |
| **Error states** | Unknown glyph type → empty string |
| **Invariants** | See below. |
| **Thread safety** | Read-only after startup. |

### Invariants

1. When `AMBER_ASCII` is set, all box-drawing characters fall back to ASCII (`+`, `-`, `|`, etc.).
2. When `AMBER_ASCII` is not set, Unicode box-drawing characters are used (U+2500–U+257F).
3. Double-width characters (emoji, CJK) are measured via `wcwidth()` — width 2, never split mid-glyph by wrapping.
4. Characters with `wcwidth() == -1` (invalid/control) are replaced with U+FFFD (width 1).

---

### Scenarios

#### [IC-01] Unicode mode — box drawing

- **Given**: `AMBER_ASCII` not set
- **Input**: `text::glyph(glyph::Set::hline)`
- **Expected**: Returns `"─"` (U+2500).
- **On failure**: Returns ASCII `-`.

#### [IC-02] ASCII mode — box drawing fallback

- **Given**: `AMBER_ASCII=1`
- **Input**: `text::glyph(glyph::Set::hline)`
- **Expected**: Returns `"-"`.
- **On failure**: Returns Unicode glyph (mojibake in ASCII terminal).

#### [IC-03] Double-width character measurement

- **Given**: CJK text `"中文"`
- **Input**: `text::display_cols("中文")`
- **Expected**: Returns 4 (each character is 2 display columns).
- **On failure**: Returns 2 (byte count, not display width).

#### [IC-04] Emoji in table cells

- **Given**: Table cell with `"🔥"`
- **Input**: Rendered via canvas
- **Expected**: `wcwidth()` returns 2 for 🔥. Column width accounts for 2. Canvas writes with `mvwaddnwstr` and advances `x` by actual drawn width.
- **On failure**: Layout breaks — columns misaligned.

#### [IC-05] Invalid UTF-8 fallback

- **Given**: Invalid UTF-8 byte sequence
- **Input**: `utf8_sanitize()` in agent_helpers
- **Expected**: Invalid bytes replaced with U+FFFD (width 1). Display continues without crash.
- **On failure**: `json::dump` throws `type_error.316`.

#### [IC-06] Markdown table borders in ASCII mode

- **Given**: `AMBER_ASCII=1`, table rendered
- **Input**: Markdown table in LLM reply
- **Expected**: Table borders render as ASCII: `|`, `-`, `+`. **This is a KNOWN GAP** — table borders are currently hardcoded UTF-8 box-drawing and do NOT check `AMBER_ASCII`.
- **On failure**: Unicode mojibake for table borders in ASCII terminal.

---

### Cross-references

- **Depends on**: `display/markdown-parser.md`, `display/table-rendering.md`, `display/ansi-parsing.md`
- **Depended on by**: `tui/layout-engine.md` (dialog borders), `tui/dialogs.md` (frame rendering)
- **Test coverage**: No direct tests.

### Known gaps

1. **Table borders ignore `AMBER_ASCII`** — Markdown tables always render Unicode box-drawing characters regardless of ASCII mode.
2. **No emoji image rendering** — Emoji rendered as text glyphs only. No image embedding.
3. **Glyph set not extensible** — All glyphs are hardcoded in `glyphs.h`. No runtime customization.
