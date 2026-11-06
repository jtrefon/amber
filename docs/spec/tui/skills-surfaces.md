# Spec: skills surfaces

- **Status:** 0.8. Second slice of the screen migration (`#517`).
- **Related:** `screen-host.md`, `dialogs.md`, `render-foundation.md`.

## 1. The explorer is a screen, not a dialog

The skills explorer used to live inside a curses `Dialog` and repaint three
panes with `mvwaddnstr`. It is a full-window screen now:

```text
SkillsBrowseCore ──► build_skills_screen ──► SkillsScreenView
                                                   │
                                          paint_skills_screen ──► Surface
                                                   │
                                            ScreenHost ──► Terminal
```

`build_skills_screen` turns the pure browse state into preformatted facet lines,
list rows, a search/status line, a tab row and a detail reference; the painter
draws them at the layout `skills_layout(cols, rows)` selects. The explorer still
owns the network, the lifecycle and the key intents — the view owns only what
the screen looks like.

## 2. Breakpoints

| Width / height | Panes |
|---|---|
| `>= 100` cols | facets column (24), list (40), detail |
| `>= 60` cols | list (34), detail |
| `< 60` cols or `< 10` rows | list only, footer explains `WxH is list-only, 60x18 + detail` |

The same breakpoint function drives the key handler (`list_height()`), so a key
that pages the list sees the rows the painter actually shows. A resize is picked
up by the host: every paint reads the terminal size and rebuilds the view.

## 3. Rows, tabs and search

- Tabs are `1 For you` … `7 Updates`; the active one uses the `Selected` role,
  a shelf with no source is `Muted`. The installed/updates shelves show the
  token budget on the right.
- The search line is `/ <query or hint>`, with `sort: <name>` and the source
  state (`idle`, `live`, `cached 4m`, `offline`, …) right-aligned. The state is
  a claim about the rows, so it never says live before a request answered.
- List rows carry the same badges as before (`•` marked, `*` installed, `+`
  update, audit mark, `?` unmanaged), the source, the install count and the
  selection bar.
- An empty shelf names its reason: no query yet, no matches, no installed
  skills, no updates, or "no source yet" for an editorial shelf.

## 4. Detail pane

`skills_detail_view` is pure: it builds a `DetailView` once per detail/file and
paints it into a `Surface` rectangle. The header is name, source, headline, the
evidence block, audit and cost; a rule, the file row and the rich Markdown body
follow. `rich_surface` maps Markdown's `rich::Line` pairs to semantic roles, so
the body keeps headings, code, quotes and rules without the painter including
ncurses.

The pane clamps its scroll to the body, and a binary or not-yet-fetched file
paints a note instead of pretending to be text.

## 5. Modal sheets

| Surface | Host | Notes |
|---|---|---|
| Install sheet | `ModalHost::custom` with `paint_install_sheet` | scope/enable marks, collision, audit and review gates, status line; `p` opens the body preview |
| Body preview / update diff | `ScreenHost` with `paint_text_screen` | full-window rich pager: `j/k`, Space, `b`, `g/G`, `/` search, `n/N`, `q`/Esc |
| Help | `ModalHost::info` | rows from `skill_key_table`, so a key cannot be documented and unhandled |
| Remove confirm | `ModalHost::confirm` | `y`/`n` or Enter/Esc; default is the safe answer |
| Facet overlay (narrow layouts) | `ModalHost::custom` over the same facet state | `j/k`, Enter toggle, `r` reset, Esc close; the column and the sheet cannot drift |

The install sheet's preview is a sequential child screen, not a second modal
stack: closing it repaints the sheet underneath. The approval countdown and the
`ModalScope` depth are unchanged.

## 6. Tests

- `tests/skills_screen_view_test.cpp` — wide/medium/narrow breakpoints, tab and
  search rows, list badges and install count, empty-shelf reasons, detail pane
  header/body/roles, install sheet marks and gates.
- `tests/tui_pty_test.cpp` — explorer open/tab/close at three sizes, the mirrored
  install → review → update → remove walk (including the install gate, `p`
  preview, pager keys and search), help, facets and the transcript fallbacks.
- `make ncurses` — `skills_explorer.cpp`, `skills_detail_view.h` and
  `skills_sheet_view.cpp` leave the include set; the ratchet drops 10 → 7.
