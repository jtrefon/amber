# Keybindings v2 migration

`keybindings.json` v2 maps a key to an **action ID** (the same one a menu item
and a slash command dispatch), a scope and an optional state predicate. v1 was a
map of internal intents. Existing files keep loading; this note is for anyone
who wants to edit them.

## What changed

| v1 | v2 |
|---|---|
| `{"alt+1": {"action": "switch_window", "arg": 0}}` | `{"key": "alt+1", "action": "core.window.set", "arg": "1"}` |
| `{"ctrl+w": {"action": "delete_word"}}` | `{"key": "ctrl+w", "action": "ui.input.delete_word", "scope": "input"}` |
| coded ESC branches | three `esc` rows with `when: drawer|busy|idle` |

`switch_window` args were zero-based; `core.window.set` is one-based, so v1
`arg: 0` becomes `arg: "1"`.

## File shape

```json
{
  "version": 2,
  "bindings": [
    {"key": "alt+1", "action": "core.window.set", "arg": "1"},
    {"key": "ctrl+w", "action": "ui.input.delete_word", "scope": "input"},
    {"key": "esc", "action": "ui.escape", "arg": "drawer", "when": "drawer"},
    {"key": "esc", "action": "ui.escape", "arg": "busy", "when": "busy"},
    {"key": "esc", "action": "ui.escape", "arg": "idle", "when": "idle"}
  ]
}
```

- `scope`: `global` (default), `input`, `menu`, `dialog`, `list`.
- `when`: `always` (default), `drawer`, `busy`, `idle`.
- `arg` is passed to the handler unchanged.
- A v1 file is translated intent by intent; ESC defaults are added because v1
  hardcoded them.

## Rules worth knowing

- The action must exist in the command tree or be a `ui.*` handler. A key cannot
  introduce an action; the conformance test checks the shipped table.
- A plugin contributes a binding only for an action its own command installs
  (`plugin.<id>.<path>`); disabling the plugin removes the command and the key.
- Core loads first and wins a `(key, scope, when)` collision; duplicates are
  counted, never silently replaced.
- `/set` and `completions.json` remain the command surface; a key is an input,
  not a second command tree.

Full contract: `docs/spec/tui/keybindings.md`.
