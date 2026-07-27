## Spec: Contextual Help

### Purpose
Provide Cisco IOS / BitchX-style contextual help at any depth in the slash
command tree. Two modes: `?` with a space before it opens a full manual page
in a scrollable popup; `?` without a space shows an inline completions popup.
The drawer itself shows namespace children with help text and choices inline.

### Ownership
- **Source files**: `tui/command_line.cpp` (`?` handling in `on_char`), `tui/tui.cpp` (result handlers), `tui/setting_registry.cpp` (man_for, children_of, choices_for), `tui/tui_render.cpp` (draw_drawer), `completions.json`
- **Data source**: `completions.json` — provides `help` (one-liner), `man` (full manual text), `children` (sub-keys), `choices` (valid values), `range` (numeric bounds)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Slash input buffer. `?` at end. |
| **Output** | Manual page popup or inline completions popup |
| **Drawer** | Shows namespace children with 34-char padded help + `[choices]` |
| **Data** | `SettingRegistry::man_for()`, `children_of()`, `help_for()`, `choices_for()`, `range_for()` |

### Help modes

| Condition | Result | What appears |
|-----------|--------|-------------|
| Space before `?` | `ShowHelpPage` | `info_dialog` with man text, children, choices |
| No space before `?` | `ShowPopup` | `menu_select` with matching completions |
| Typing `/get` | Drawer | Children of "get" with help text |
| Typing `/get policy` | Drawer | Children of "policy" with help text |
| Typing `/get think` | Drawer | "think" with help text + `[on\|off\|auto]` |

### Manual page content

When `?` is pressed after a space, the handler:
1. Looks up `man_for(path)` — full description text
2. Shows one-liner `help_for(path)` as subtitle
3. Lists `children_of(path)` each with their `help_for` text
4. Appends `choices_for(path)` or `range_for(path)` for leaf settings

### Drawer format

```
/get →
  config    Open the settings configuration screen.
  think     Show the thinking/reasoning display mode.  [on|off|auto]
  policy    Permission and approval settings.
  ...
```

Children are padded to 34 characters, followed by two spaces and the
one-line help text. Choice-type settings show `[choices]` inline. Range-type
settings show `[lo-hi]` inline.

---

### Scenarios

#### [CH-01] `?` at root — manual page for commands
`/ ?` → info_dialog with man text for root command tree.

#### [CH-02] `?` after command — manual page for that command
`/get ?` → info_dialog with man text + children listing for "get"
(config, model, provider, toolfold, policy, display, think, detection,
compression).

#### [CH-03] `?` at nested namespace
`/set policy ?` → info_dialog with man text for "policy" +
children: mode, approval, timeout (each with help text).

#### [CH-04] `?` at leaf setting — choices and range
`/get think ?` → info_dialog with man text + choices: on, off, auto.
`/set policy timeout ?` → info_dialog with range: 0-999.

#### [CH-05] Drawer at root
`/` → drawer shows all root commands with help text.

#### [CH-06] Drawer at namespace
`/get` → drawer shows children of "get" with help text.

#### [CH-07] Drawer auto-descends on exact match
`/get policy` → drawer descends into policy namespace,
shows mode, approval, timeout with help text.

#### [CH-08] Drawer at leaf with choices
`/get think` → drawer shows "think" with help + [on|off|auto].

#### [CH-09] Tab completion strips namespace prefixes
`/get policy` Tab → completes to `/get policy ` showing
"mode", "approval", "timeout" as shadow candidates (not
"policy.mode", "policy.approval", "policy.timeout").

---

### Cross-references
- **Depends on**: `completions.json` (data), `tui/setting_registry.cpp` (lookups)
- **Depended on by**: `tui/input-system/slash-engine.md`
