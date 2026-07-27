## Spec: TUI Contextual Help

### Purpose
Provide contextual help at any depth using `?` in two modes:

| Typed | Behaviour |
|-------|-----------|
| `/set detection loop?` | No space before `?` → quick popup of remaining options at current position (inline help for what you're typing) |
| `/set detection loop ?` | Space before `?` → full help page for the resolved path before `?` |

The space is the differentiator. No space = "help for this token" (remaining
options popup). Space = "help for everything before `?`" (full page). This
matches Cisco IOS (both modes) and removes the need for `/help` entirely.

`/help` is kept as a shorthand for `?` at root.

### Inspiration
- **Cisco IOS `?`**: type `?` at any config mode depth — no space shows
  remaining options for the current word; space before `?` confirms the word
  and shows help for the completed context
- **BitchX**: drawer updates as you type; typing `/set ` shows available keys
- **Key insight**: `/help <path>` forces retyping the path. `?` at depth
  reuses what you already typed. Space vs no-space gives two levels of help.

### Ownership
- **Source files**: `tui/tui_input.cpp` (`/help` → delegate to `?` handler),
  `tui/tui_render.cpp` (drawer help header, popup renderer)
- **Target**: `?` is intercepted at any depth

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Typed `?` — with space before it (full help page) or without space (remaining options popup) |
| **Output** | Full help page (info_dialog) for space-before-`?`. Quick popup of remaining valid tokens for no-space-`?`. |
| **Error states** | Unknown path before `?` → error message. |
| **Thread safety** | Main thread only. |

### Invariants

1. **`?` with space before it** (e.g., `/set detection loop ?`): engine
   resolves the path before the space and shows the full help page for that
   node. The `?` is consumed — not passed to the handler.
2. **`?` without space before it** (e.g., `/set detection loop?`): engine
   checks if the input BEFORE `?` would be a valid partial command. If so,
   shows a quick popup of remaining options for the current position. If not,
   `?` is treated as a literal character (e.g., `/set val?` passes `val?`).
3. `/help [path]` is syntactic sugar — delegates to the appropriate `?` mode.
4. The full help page shows: usage synopsis, description, subcommands (if any),
   positional args with types/choices/placeholders, current values, flags,
   related commands, and aliases.
5. The quick popup shows: valid next tokens with brief descriptions and
   current values. No full help content. Designed for quick reference.
6. After dismissing either help type, the `?` is removed from input and the
   cursor returns to the position before `?`.

---

### Scenarios

#### [CH-01] `?` with space at root — full help page

- **Given**: User types `/ ?`
- **Input**: Enter (space before `?` triggers full page)
- **Expected**: Full help page showing all top-level commands: name, args, description, current value (if applicable). Table-formatted. Key bindings at bottom.
- **On failure**: Empty dialog.
- **Note**: `/help` without args is equivalent.

#### [CH-02] `?` with space at command level — full page

- **Given**: User types `/set ?`
- **Input**: `?` with space before it
- **Expected**: Full help page for `set`:
  ```
  /set <key> <value>  — Set runtime options
  
  Subcommands:
    detection    <loop|duplicate> <on|off|toggle>  Detection settings
    display      markdown <on|off>                 Display settings  (markdown: on)
    toolfold     <always|auto|never>               Tool result folding  (auto)
    ...</i></i>
  
  Aliases: (none)
  Related: /get
  ```
- **On failure**: Single-line response without depth.

#### [CH-03] `?` with space at nested depth — full page

- **Given**: User types `/set detection loop ?`
- **Input**: `?` with space before it
- **Expected**: Full help page for `loop`:
  ```
  /set detection loop <on|off|toggle>  — Tool loop detection
  
  Description: When enabled, the agent detects repeated identical tool call
  sequences and stops the loop with a descriptive error message.
  
  Current value: off
  
  Arguments:
    value  <on|off|toggle>  required  — on=enable, off=disable, toggle=flip
  
  Related: /set detection duplicate, /get detection loop
  ```
- **On failure**: Shows parent help instead.

#### [CH-04] `?` without space at value position — inline options popup

- **Given**: User types `/set detection loop?` (no space)
- **Input**: `?` is typed immediately after `loop`
- **Expected**: Quick popup showing remaining options for the current position:
  ```
  on      Enable loop detection
  off     Disable loop detection
  toggle  Flip the current state
  ```
  This is a small overlay — not a full help page. Dismiss with Esc.
  The `?` is stripped from `loop?` → input becomes `/set detection loop `.
- **On failure**: `?` treated as literal character ("loop?" passed as value).

#### [CH-05] `?` without space at command level — inline options popup

- **Given**: User types `/set` then `?` immediately (no space)
- **Expected**: Quick popup showing subcommands of `set`:
  ```
  detection   Detection settings
  display     Display settings
  toolfold    Tool result folding
  ...
  ```
  Dismiss with Esc. Input stays `/set` (no space inserted).

#### [CH-06] `?` in key-chain mode — with space, full page

- **Given**: User types `/config network proxy ?` (space before `?`)
- **Expected**: Full help page showing key-chain context:
  ```
  /config <key>... <value>  — Arbitrary configuration keys
  
  Key path so far: network / proxy
  Expected value: <string> (e.g., http://proxy:8080)
  ```

#### [CH-07] `?` in key-chain mode — without space, inline popup

- **Given**: User types `/config network proxy?` (no space)
- **Expected**: Quick popup showing value spec for the key-chain:
  ```
  <value: string>  — Proxy URL (e.g., http://proxy:8080)
  ```

#### [CH-06] Keyboard `?` shortcut — quick popup

- **Given**: User has typed `/set ` (no trailing space needed)
- **Input**: Keyboard shortcut (Ctrl+? or mapped key)
- **Expected**: Quick popup showing remaining options for the current position.
  Smaller than a full help page — designed for quick reference.
  E.g. at `/set detection loop `, shows:
  ```
  on      Enable
  off     Disable
  toggle  Flip
  ```
- **On failure**: Full help page instead of quick popup, or no response.

#### [CH-07] `?` at unknown path — error

- **Given**: User types `/set foo ?`
- **Input**: `?` as last token
- **Expected**: Resolves `set` → tries `foo` → not found → error: `"unknown command: /set foo"`. Help does not fire.
- **On failure**: Help page for `set` (parent) instead of error.

#### [CH-08] `/help` as alias for `?` at root

- **Given**: User types `/help`
- **Input**: Enter
- **Expected**: Resolves `help` as a command → delegate to `?` at root → same as `/ ?`.
- **Given**: User types `/help set`
- **Input**: Enter
- **Expected**: Delegate to `?` at `set` path → same as `/set ?`.
- **On failure**: `/help` treated as separate command with hardcoded output.

#### [CH-09] Drawer header and `?` hint

- **Given**: Drawer open at any depth
- **Expected**: Header shows: `...  ?: help at this level  ...`
- **On failure**: No `?` hint in drawer.

#### [CH-10] Help page for value type

- **Given**: User types `/set compression threshold ?` (threshold expects Float)
- **Expected**: Help page:
  ```
  /set compression threshold <float> — Context utilisation threshold
  
  Description: Fraction of the context window that must be used before
  automatic compression triggers.
  
  Current value: 0.75 (default: 0.50)
  Type: float
  Range: [0.10 — 1.00]
  
  Related: /set compression min_turns
  ```

#### [CH-11] Help page for choice type

- **Given**: User types `/set policy ?` (policy expects Choice)
- **Expected**: Help page:
  ```
  /set policy <read|write|yolo>  — Agent mode
  
  Current value: write
  
  Choices:
    read   Read-only mode — only read/search tools available
    write  Write mode — all tools, auto-approve (default)
    yolo   YOLO mode — all tools, no approval, full system access
  
  Related: /get policy
  ```

#### [CH-12] Help page — flags section

- **Given**: A command with flags, user types `/session save ?`
- **Expected**: If `session save` has flags, they are shown:
  ```
  /session save <name> — Save current session
  
  Arguments:
    name  <string>  required  — Session file name
  
  Flags:
    --all         Save all windows, not just active
    --no-tools    Exclude tool call history
    --json        Output in JSON format
  
  Related: /session load, /session list
  ```

#### [CH-13] Help dismissal — `?` stripped from input

- **Given**: User types `/set detection ?`, help page displayed
- **Input**: User presses Esc or any key to dismiss
- **Expected**: Help page closes. Input restored to `/set detection ` (without ` ?`). Cursor at end. User can continue typing.
- **On failure**: Input cleared or `?` remains.

---

### Cross-references

- **Depends on**: `tui/input-system/slash-engine.md`, `tui/input-system/auto-complete.md`, `tui/input-system/nested-commands.md`
- **Depended on by**: `tui/event-loop.md`
- **Test coverage**: `tests/tui_tests.cpp` — `usage()` test. Comprehensive help tests needed.

### Known gaps

1. **`?` at key-chain intermediate depth** — For `/config network proxy ?`, the key path `network/proxy` is known but only the value spec can be shown. Key-chain nodes with deeper known structure need special handling.
2. **No man-page style help (current)** — All help is structured text. No multi-paragraph documentation or examples per command.
3. **Related commands are static (current)** — Should be derived from tree proximity (siblings, parent, common prefixes) rather than hardcoded.
