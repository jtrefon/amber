# Spec: keybindings — key to action ID

- **Status:** 0.8. `keybindings.json` v2 is data: a key maps to an action ID, a
  scope and an optional state predicate. The host runs the action through the
  one `ActionRegistry` the menus and slash commands already use.
- **Issue:** #515.
- **Related:** `command-tree.md`, `command-shortcuts.md`, `menus.json`,
  `docs/spec/plugins/developer-guide.md` (the `Keybinding` capability).

## 1. Why an action ID

A key is an input, not a feature. The binding table names the same action the
menu item and the slash command dispatch, so there is one handler, one place to
check whether it is allowed, and no second command surface hiding behind a key.
`ui.*` actions are the exception only in that they are internal: they still live
in the same registry.

The chain is:

```text
raw key -> KeyBinder.dispatch -> agent::KeyBinding{action, arg}
        -> SlashDispatcher::dispatch_key_action -> ActionRegistry
        -> handler (core.*, ui.*, plugin.*)
```

`KeyBinder` is pure (no ncurses, no dispatch). It reads a `KeyRead` plus an
`InputState` snapshot and returns the binding; the host owns the terminal I/O,
the redraw and the input context.

## 2. File

`keybindings.json` is resolved exactly like `completions.json`: a bundled copy
beside the executable or in the installed data directory, then the workspace
copy. A missing or malformed file leaves the table empty — the host does not
guess keys.

```json
{
  "version": 2,
  "bindings": [
    {"key": "alt+1", "action": "core.window.set", "arg": "1", "scope": "global"},
    {"key": "ctrl+w", "action": "ui.input.delete_word", "scope": "input"},
    {"key": "esc", "when": "busy", "action": "ui.escape", "arg": "busy", "scope": "global"}
  ]
}
```

### Fields

| Field | Required | Meaning |
|---|---|---|
| `key` | yes | terminal-level spelling: `ctrl+<a-z>`, `alt+<0-9>`, `alt+<a-z>`, `esc+<a-z>`, `esc+<1-9>`, or `esc` |
| `action` | yes | an action ID present in the command tree, or a `ui.*` handler |
| `arg` | no | argument passed to the handler unchanged |
| `scope` | no | `global` (default), `input`, `menu`, `dialog`, `list` |
| `when` | no | `always` (default), `drawer`, `busy`, `idle` |

`global` matches every input surface. A scoped binding matches only that
surface, so a menu-local key and a prompt-local key may share a spelling. When
several bindings share a key, the first matching one wins and a later binding
with the same key/scope/predicate is refused and counted (`collisions()`).

### State predicates

| `when` | True when |
|---|---|
| `always` | the key arrived |
| `drawer` | the command drawer is open |
| `busy` | the active window's agent is running |
| `idle` | the drawer is closed and the agent is not running |

The shipped ESC rows use all three states: `drawer` closes the drawer, `busy`
cancels the run, `idle` toggles scroll mode. They are three bindings of one key,
distinguished by the predicate rather than by a branch in the input loop.

## 3. Scopes

`InputState::scope` names the local loop that read the key. The prompt sets
`input`; a modal menu or dialog sets its own scope while it owns the keyboard.
The same table therefore describes keys that have nothing to do with the
prompt, and a plugin can contribute a `menu` binding without knowing where the
prompt is.

## 4. One registry

- `core.*` and `ui.*` handlers are registered once at startup by
  `SlashDispatcher::register_builtin_actions()` (grouped by domain:
  `register_core_task_actions`, `register_tab_actions`, `register_view_actions`,
  `register_input_actions`).
- Menu items are JSON (`menus.json`) and dispatch the same IDs.
- A plugin contributes `KeybindingCapability`; the runtime installs it into the
  `KeybindingRegistry` inside the plugin ledger, so disabling the plugin unwinds
  its keys with everything else.
- The action must already have a handler through the plugin's own command
  contribution. A keybinding cannot introduce an action, and cannot claim a
  core action: the host derives plugin actions as `plugin.<id>.<path>`.
- The core `keybindings.json` table is merged first; a plugin binding that
  repeats a key/scope/predicate is refused and counted. Core wins by
  construction, not by load order luck.

## 5. v1 compatibility

The previous file was a map of internal intents:

```json
{"alt+1": {"action": "switch_window", "arg": 0}}
```

It still loads. The seven intents the coded binder understood translate to
action IDs (`switch_window` gains 1 for the one-based `core.window.set`,
`delete_word` becomes `ui.input.delete_word` with `scope: input`, and so on).
ESC handling was coded in v1 and never table-driven, so those files get the same
ESC defaults they already had. An unknown intent is skipped rather than invented.

## 6. Conformance

`tests/menu_test.cpp` checks, from the real `keybindings.json` and
`completions.json`:

1. every binding action resolves — a documented command-tree action or a known
   `ui.*` handler;
2. every scope and `when` is one of the declared values;
3. no two bindings share a (key, scope, when) triple;
4. `collisions()` is zero.

`tests/plugin_runtime_test.cpp` checks the other half: the bundled hello
plugin's `ctrl+y` binding names `plugin.hello.greet`, that action is installed
in the `ActionRegistry` beside its command, it dispatches, and disabling the
plugin removes both the command and the key. `tests/tui_pty_test.cpp` drives the
key end to end.

## 7. Acceptance

- A menu item and its slash command run the same action ID.
- F10 and the amber mark toggle the menu; Esc unwinds (menu spec).
- A hint never enters the input buffer and never submits (menu spec).
- A key with no binding falls through to the input line; a bound key does not
  insert its character.
- A plugin key works after enable and stops after disable with no restart.
