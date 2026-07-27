# Help System Refactor — Scope & Architecture

## Problem

The TUI slash-command system has no usable contextual help. A user who types `/`
sees a flat list of root commands, but once they enter a namespace (e.g. `/get`,
`/set policy`) the drawer and `?` key stop giving useful information. The data
for rich help exists in `completions.json` and `SettingRegistry` but is never
rendered as namespace-aware help.

## Goals

1. **Namespace drawer** — typing `/get` replaces the root command list in the
   drawer with all children of the `get` namespace, each showing its one-line
   help text.
2. **Manual pages** — `?` at any point in a slash path shows a full manual page
   (name, description, choices, impact) for the current namespace node.
3. **Every level works** — `/`, `/get`, `/get policy`, `/set`, `/set policy`,
   `/set policy rule`, etc. all provide drawer completions and `?` manual pages.
4. **No UX regressions** — Tab completion, Enter dispatch, Up/Down selection,
   Esc cancel all continue working exactly as before.

## Architecture

### Data layer — `completions.json`

Add a `"man"` field to every node that already has `"help"`:

```json
{
  "commands": {
    "get": {
      "help": "Retrieve system/app state information.",
      "man": "The get command retrieves current values of runtime settings...",
      "children": {
        "policy": {
          "help": "Permission and approval settings.",
          "man": "Policy controls how the agent interacts with tools...",
          "children": {
            "mode": {
              "help": "Agent access mode (read/write/yolo).",
              "man": "The mode controls what tools are available..."
            }
          }
        }
      }
    }
  }
}
```

### Registry — `SettingRegistry`

| Method | Status | What it does |
|--------|--------|-------------|
| `help_for(key)` | ✅ Exists | Returns one-liner for drawer listing |
| `man_for(key)` | ❌ New | Returns full manual text for `?` pages |
| `children_for(key)` | ❌ New | Returns child keys of a namespace node from JSON |
| `load_completions_json()` | ⚠️ Extend | Index `man` fields alongside `help`; index namespace roots too |

### Drawer — `draw_drawer()` in `tui_render.cpp`

**Current behavior:** Shows flat command list from `palette::filter()` when input
starts with `/`. Once a space is typed (arg mode), shows one-line usage for the
matched command.

**New behavior:**

| Input state | Current | New |
|-----------|---------|-----|
| `/` only | All root commands | Same (no change) |
| `/get` | All commands matching "get" | Children of `get` namespace: `mode`, `approval`, `timeout`, `policy`... each with `help` text |
| `/get policy` | One-line usage for `get` | Children of `get.policy`: `mode`, `approval`, `timeout`... |
| `/get policy mode` | One-line usage for `get` | Choices/range for the leaf setting |
| `/set policy rule ` | One-line usage for `set` | N/A (dynamic names, can't enumerate) |

**What changes:**
- `draw_drawer()` needs the current slash path (e.g. `"get"`, `"set.policy"`)
- It calls `SettingRegistry::children_for(path)` to get sub-keys
- Falls back to `palette::filter()` when no namespace children exist (root commands, dynamic args)
- Each child row shows: name + help text

### Manual page dialog — new `InfoPanel` reuse

The existing `InfoDialog` (`tui/info_dialog.cpp`) is a scrollable text viewer
used for config screens. We can reuse it for man pages:

| Trigger | Action |
|---------|--------|
| `/set ?` | Show man page for `set` namespace |
| `/get policy ?` | Show man page for `policy` |
| `/set policy rule ?` | Show man page for `rule` |
| `/stop ?` | Show man page for `stop` |

The man page is assembled from:
- `man_for(path)` — full description from JSON
- `help_for(path)` — one-liner (shown as subtitle)
- `choices_for(path)` — if leaf, show valid choices
- `range_for(path)` — if numeric, show range

### `?` key handling — `CommandLine` + `Tui::run()`

**Current:** `on_char('?')` returns `ShowHelpPage` or `ShowPopup` depending on
whether there's a space before the `?`.

**New:** Always return a new `ShowManPage` result carrying the current path. The
handler in `Tui::run()` opens an `InfoDialog` with the assembled man content
instead of appending a status line.

### Cleanup opportunities

| Component | What | Why |
|-----------|------|-----|
| `palette::usage()` | Formatting helper for command usage | May become redundant if drawer renders from JSON |
| `palette::filter()` | Flat command matching | Still needed for root level, less important once drawer uses namespace children |
| `cmd_help()` | Legacy text-based help | Can be simplified to delegate to man page system |
| `ShowHelpPage` result | One-line help display | Replaced by `ShowManPage` for namespaces; kept for leaf settings |
| `ShowPopup` result | `?` popup for completions | Replaced by manual page for namespaces |

## Files to modify

| File | Changes |
|------|---------|
| `completions.json` | Add `"man"` field to every node; ensure every namespace has `"help"` |
| `tui/setting_registry.h` | Add `man_for()`, `children_for()`, `ManMap` member |
| `tui/setting_registry.cpp` | Implement new methods; extend `load_completions_json()` to index man text and namespace roots |
| `tui/tui_render.cpp` | Rewrite `draw_drawer()` to show namespace children with help text |
| `tui/tui.cpp` | Update `?` handler to use `ShowManPage` + `InfoDialog`; update `update_completions()` for namespace path |
| `tui/tui_input.cpp` | Simplify `cmd_help()`; ensure `build_settings()` settings have `help` text |
| `tui/command_line.h` | Maybe expose current namespace path |
| `tui/command_line.cpp` | Maybe update `recompute()` for namespace detection |

## Implementation order

```
1. completions.json     — add man fields (30 min)
2. SettingRegistry      — add man_for, children_for, extend loader (45 min)
3. Tui::run() ? handler — wire man page dialog (30 min)
4. draw_drawer()        — namespace-aware child listing (1.5 h)
5. Tui::update_completions — ensure completions flow from namespaces (30 min)
6. Cleanup              — simplify cmd_help, remove dead palette paths (30 min)
7. Test & verify        — run tests, manual UX check (30 min)
```

Total estimated effort: **~4 hours**
