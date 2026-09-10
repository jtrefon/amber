# TUI Issues — Proposal

## Problem

Two TUI bugs identified in the architectural review:

### 1. UTF-8 byte/display-column mismatch in input rendering

`RenderEngine::draw_input()` computes cursor position and scroll offset
in **display columns** (via `display_cols()`), but slices the input string
using **byte offsets**. For multi-byte UTF-8 input (CJK, emoji), the byte
offset and display column diverge, causing:

- Wrong portion of the string displayed when scrolled
- Cursor positioned at the wrong column
- Shadow text rendered at the wrong position

The `put` lambda also advances `x` by `ws.size()` (number of wide
chars), which is incorrect for double-width characters (CJK/emoji take
2 columns each).

Additionally, `text::wrap()` counts each UTF-8 character as 1 column
(`++cols` at line 205), but `display_cols()` correctly uses `wcwidth()`.
A 2-cell-wide CJK character is counted as 1 column in wrapping, causing
lines to overflow the configured width.

### 2. os.* vs core.* slash-command action mismatch

`completions.json` defines `os.files.*` and `os.system.*` actions for
the `/files` and `/system` command trees. But `tui_input.cpp` registers
handlers as `core.files.*` and `core.system.*`. When a user types
`/files ls`, the tree walk finds `action: "os.files.ls"`, but the action
registry only has `core.files.ls` — so dispatch fails with "no handler
for this action".

## Target state

### 1. Fix UTF-8 column handling

**`draw_input()`**: Use `text::col_to_byte()` to convert display-column
offsets to byte offsets when slicing the input string. Advance `x` by
`display_cols()` instead of `ws.size()` in the `put` lambda.

**`wrap()`**: Use `display_cols()` for the running column count instead
of counting characters.

### 2. Fix action namespace mismatch

Align the C++ handler registrations with the JSON tree. The JSON tree
is the single source of truth (per AGENTS.md), so change the C++
registrations from `core.files.*` / `core.system.*` to `os.files.*` /
`os.system.*`.

## Scope

- `tui/render_engine.cpp`: fix `draw_input()` to use `col_to_byte()`
  and `display_cols()` for column tracking
- `tui/textutil.cpp`: fix `wrap()` to use `display_cols()` for column
  counting
- `tui/tui_input.cpp`: rename `core.files.*` → `os.files.*` and
  `core.system.*` → `os.system.*` handler registrations
- `tests/tui_tests.cpp`: add regression tests for UTF-8 column handling
  and action dispatch

## Risk

Low. The `col_to_byte()` helper already exists and is tested. The
action rename is mechanical (string replacement). The `wrap()` fix
changes line-breaking behavior for wide characters only — ASCII text
is unaffected.
