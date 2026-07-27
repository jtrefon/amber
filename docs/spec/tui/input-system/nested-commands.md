## Spec: TUI Nested Commands (Tree + Key-Chain Model)

### Purpose
Handle command hierarchies of arbitrary depth. Two resolution patterns are
supported, tried in order:

1. **Fixed tree** (Cisco IOS style): each token resolves a predefined
   `CommandNode` with known subcommands, typed positional args, and flags.
   E.g. `/session save mysession` where `session` → `save` → `mysession` (ArgSpec:String).

2. **Key-value chain** (dotted-path style): after resolving the fixed prefix,
   remaining tokens are treated as `key key key ... value` pairs where the
   depth is not predefined. E.g. `/config network proxy http://proxy:8080`
   where `config` is fixed, then `network proxy` are dynamic keys, then
   `http://proxy:8080` is the value.

This replaces the current flat `Command` struct with manual string parsing.
The target architecture mirrors the standalone `completion/` library but adds
key-chain support.

### Inspiration
- **Cisco IOS**: nested command hierarchy with both fixed subcommands and
  dynamic key-value pairs at arbitrary depth
- **Consul KV / etcd**: `/config/service/environment/key value` — arbitrary
  key path
- **zsh completion**: context-sensitive completion that knows the type of
  each positional argument
- **BitchX `/set`**: deep key-value settings with tab completion per level

### Ownership
- **Source files**: `tui/tui_input.cpp` (`cmd_set()` → flat string parsing, `cmd_get()`, `cmd_policy()`, `cmd_model()`, `cmd_provider()`), `completion/` library (standalone, unused — reference implementation)
- **Target**: Replace flat `Command` with `Subcommand` tree model

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Full slash command path (e.g., `/set detection loop on`) |
| **Output** | Tree walk → leaf handler invoked with resolved args. Config mutation + UI updates. |
| **Error states** | Unknown subcommand → error at the level it failed. Invalid arg type → error with description. Missing required arg → error listing required args. |
| **Thread safety** | Main thread only. |

### Target Command Tree Structure

```
help [command]                          — Show help (sugar for `?` at depth)

# Settings
set                                     — Runtime settings
  detection                              — Detection settings
    loop <on|off|toggle>                 — Tool-loop detection
    duplicate <on|off|toggle>            — Duplicate call detection
  display                                — Display settings
    markdown <on|off>                    — Markdown rendering toggle
  toolfold <always|auto|never>           — Tool result folding
  policy <read|write|yolo>               — Agent mode
  model <name>                           — Set active model
  think <on|off|auto>                    — Thinking mode
  compression                            — Compression settings
    threshold <0.1-1.0>                  — Context utilisation threshold
    min_turns <1-999>                    — Min turns before compression
get [key]                               — Show current setting(s)

# Provider management (CRUD)
provider                                — Provider management
  list                                   — List saved providers
  add <name>                             — Add new provider
  edit <name>                            — Edit provider settings
  delete <name>                          — Delete saved provider
  test <name>                            — Test connection

# Model management
model                                   — Model management
  list                                   — List models from server
  set <name>                             — Set active model
  probe                                  — Probe server for model info

# Session management (CRUD)
session                                 — Session management
  list                                   — List saved sessions
  save [name]                            — Save current session
  load <id>                              — Load session
  delete <id>                            — Delete session
  rename <id> <title>                    — Rename session

# Job management (CRUD)
job                                     — Background job management
  list                                   — List running/finished jobs
  start <command>                        — Start new job
  read <id> [all]                        — Read job output
  kill <id>                              — Kill job

# File browsing (CRUD-style)
files                                   — File browsing
  ls [path]                              — List directory contents
  tree [path]                            — Show directory tree
  open <path>                            — View file in pager
  find <pattern> [path]                  — Find files by name

# System operations
system                                  — System operations
  exec <command>                         — Execute shell command and capture output.
                                         Output can be injected into agent context
                                         (e.g., man pages, system info → agent sees
                                         it as context). No stdin interaction (v1).
                                         Future: stdin pass-through.
  delete <path>                          — Delete file or empty directory
  rmdir <path>                           — Delete directory tree (rm -rf)
  mkdir <path>                           — Create directory
  mv <src> <dst>                         — Move/rename
  cp <src> <dst>                         — Copy
  info <path>                            — File/directory info (stat)
  ps                                     — List processes
  kill <pid>                             — Kill process by PID
  df [path]                              — Disk usage
  uptime                                 — System uptime
  uname                                  — System information

# Control
stop [cancel|kill]                      — Stop agent
compress [compact]                      — Trigger compression
save                                    — Save current session (shorthand)
quit [exit|q]                           — Exit

# Future
config <key>... <value>                 — Arbitrary config key chain
```

### Shortcut aliases (BitchX / IrcII glued shorts)

Every command node has an optional `aliases` vector for shortcuts. The alias
scheme follows BitchX (single-letter for top-level) and IrcII (glued two-letter
for nested commands):

| Alias | Expands to | Rationale |
|-------|-----------|-----------|
| `/h` | `/help` | Single letter, obvious |
| `/q` | `/quit` | BitchX standard |
| `/s` | `/save` | Single letter, common |
| `/c` | `/compress` | Single letter |
| `/st` | `/stop` | Glued, `s` taken by save |
| `/sl` | `/session list` | Glued, `session l` without space |
| `/ss` | `/session save` | Glued |
| `/sd` | `/session delete` | Glued |
| `/sr` | `/session rename` | Glued |
| `/j` | `/job` | Top-level shortcut |
| `/jl` | `/job list` | Glued |
| `/jk` | `/job kill` | Glued |
| `/jr` | `/job read` | Glued |
| `/js` | `/job start` | Glued |
| `/f` | `/files` | Top-level shortcut |
| `/fl` | `/files list` | Glued |
| `/ft` | `/files tree` | Glued |
| `/fo` | `/files open` | Glued |
| `/ff` | `/files find` | Glued |
| `/m` | `/model` | Top-level shortcut |
| `/ml` | `/model list` | Glued |
| `/p` | `/provider` | Top-level shortcut |
| `/pl` | `/provider list` | Glued |
| `/pa` | `/provider add` | Glued |
| `/pe` | `/provider edit` | Glued |
| `/pd` | `/provider delete` | Glued |
| `/pt` | `/provider test` | Glued |
| `/sy` | `/system` | Top-level shortcut (`s` taken) |
| `/sye` | `/system exec` | Glued |
| `/syd` | `/system delete` | Glued |
| `/syr` | `/system rmdir` | Glued |
| `/sym` | `/system mkdir` | Glued |
| `/syv` | `/system mv` | Glued |
| `/syc` | `/system cp` | Glued |
| `/syi` | `/system info` | Glued |
| `/sys` | `/system ps` | Glued (`sy` + `s` for processes) |
| `/syk` | `/system kill` | Glued |
| `/syu` | `/system uptime` | Glued |
| `/syn` | `/system uname` | Glued |

**Design rules for aliases:**

1. Top-level commands with unique first letter get a single-letter alias
   (e.g., `/q` for `quit`, `/s` for `save`, `/c` for `compress`).
2. Top-level commands without a unique first letter use a unique two-letter
   prefix (e.g., `/st` for `stop`, `/sy` for `system`).
3. Nested commands use glued shortcuts: first letter of parent + first letter
   of child (e.g., `/sl` = `s`ession + `l`ist, `/fl` = `f`iles + `l`s).
4. If the glued pair conflicts, use the next unique letter from the child
   (e.g., `/fo` = `f`iles + `o`pen, since `/fl` is taken by list).
5. The alias resolution is depth-first in the tree — `/st` resolves to `stop`,
   not to some hypothetical `s` subcommand with `t` sub-subcommand.
6. Aliases appear in the autosuggest shadow and drawer alongside the full name.
   The shadow prefers the full name for clarity; Tab accepts the full expansion.
7. Users can type the alias OR the full name interchangeably.

**Implementation:** In the `CommandNode` struct, `aliases` stores the shortcut
strings. The tree walker checks aliases at each depth level.
```cpp
CommandNode quit = {
    .name = "quit",
    .aliases = {"q", "exit"},
    .description = "Exit the application",
    .run = cmd_quit
};
```

### Node Model

Two resolution modes, tried in order at each node:

#### Mode 1: Fixed Tree Resolution

```
For each remaining token:
  1. Try to match against subcommands by name or alias.
  2. If matched, recurse into that subcommand with the next token.
  3. If not matched, proceed to Mode 2.
```

#### Mode 2: Key-Chain Resolution (fallback)

When no subcommand matches and `allow_key_chain = true`:

```
1. Collect all remaining tokens into a key path.
2. The LAST token must match the ValueSpec (typed, e.g. Choice or String).
3. Dispatch handler with {keys: [token1, token2, ...], value: lastToken}.
```

This allows arbitrary depth: `/config network proxy http://proxy:8080`
where `config` is fixed, `network proxy` are key-chain tokens, and
`http://proxy:8080` is the typed value.

```cpp
enum class ArgType { String, Choice, Integer, Float, Bool, Path, Json, Any };

struct ArgSpec {
    std::string name;              // display name for help/completion
    ArgType type = ArgType::String;
    std::vector<std::string> choices;           // for Choice type
    std::pair<double,double> range = {0, 0};   // min/max for numeric types
    std::string placeholder;                    // e.g., "<0.1-1.0>", "<name>"
    std::string description;
    bool required = true;
};

struct FlagSpec {
    std::string name;              // e.g., "verbose"
    char short_flag = '\0';        // e.g., 'v'
    std::string description;
    std::function<bool()> is_active; // runtime check, e.g. --debug
};

struct CommandNode {
    // Identity
    std::string name;
    std::vector<std::string> aliases;
    std::string description;

    // Mode 1: Fixed tree resolution
    std::vector<CommandNode> subcommands;
    std::vector<ArgSpec> args;                // positional args for THIS level
    std::vector<FlagSpec> flags;              // optional --flags

    // Mode 2: Key-chain resolution (fallback when no subcommand matches)
    bool allow_key_chain = false;             // accept unknown tokens as keys
    int  min_keys = 0;                        // minimum keys before value
    ArgSpec value_spec;                       // type of the final value

    // Display & dispatch
    std::function<std::string()> current_value;  // evaluated at render time
    std::function<void(const ParsedCommand&)> run;
};

// What gets passed to the handler:
struct ParsedCommand {
    std::vector<std::string> path;       // resolved subcommand path
    std::map<std::string, std::string> flags;      // --key=value
    std::vector<std::string> positional_args;       // matched ArgSpec values
    std::vector<std::string> key_chain;             // key-chain tokens
    std::string value;                              // final value (key-chain mode)
    bool no_prefix = false;              // /no prefix was used (invert action)
};
```

### Resolution Algorithm

```
resolve(tokens, node):
  // Step 0: Cisco IOS `/no` prefix — invert the command
  no_prefix = false
  if tokens not empty and tokens[0] == "no":
    no_prefix = true
    tokens = tokens[1:]  // strip `no`, continue resolving

  flags = extract_flags(tokens, node.flags)
  remaining = tokens - flags

  // Mode 1: fixed subcommand
  if remaining not empty:
    sub = find_subcommand(remaining[0], node.subcommands)
    if sub:
      result = resolve(remaining[1:], sub)
      if result.ok:
        result.no_prefix = no_prefix
        return result

  // Mode 1 continued: positional args at this level
  if node.args not empty:
    matched = match_args(remaining, node.args)
    if matched.ok:
      return success { path: node.path, args: matched.values, flags, no_prefix }

  // Mode 2: key-chain fallback
  if node.allow_key_chain && remaining.size() >= node.min_keys + 1:
    value = remaining.back()
    if matches_spec(value, node.value_spec):
      keys = remaining[0..-2]
      return success { key_chain: keys, value, flags, no_prefix }

  // No resolution
  if remaining empty:
    return show_frame(node)   // show subcommands + current values
  else:
    return error("unknown: " + remaining[0], node)
```

### Invariants (Target)

1. Tree resolution is tried first; key-chain fallback only when no subcommand matches.
2. Flags (`--flag`) can appear at any depth and are extracted before resolution.
3. Unknown tokens at any depth produce an error pinpointing the failure.
4. Completion at each level provides only the valid next tokens for that depth.
5. `?` at each level shows only the valid completions for that depth.
6. In key-chain mode, `?` shows the value spec (type, choices, range).
7. `current_value` is dynamic — evaluated at render time, shows live state.
8. Key-chain mode can represent arbitrary nesting without recompiling the command tree.

---

### Scenarios

#### [NC-01] `/set detection loop on` — three-level deep

- **Given**: Tree: `set` → `detection` → `loop` (ArgSpec: Choice `on|off|toggle`)
- **Input**: `/set detection loop on`
- **Expected**: Tree walk: `find("set")` → `find_subcommand("detection")` → `find_subcommand("loop")` → match arg `"on"` to Choice → handler invoked. `cfg_.detection_loop = true`. Status: `"detection loop: on"`.
- **On failure**: Partial match stops with `"unknown command: /set detection foo"`.

#### [NC-02] `/set` — list subcommands (no arg)

- **Given**: User types `/set` with no arguments
- **Input**: Enter
- **Expected**: Tree walk finds `set`. No subcommand or arg token provided → `show_command_frame()` displays all subcommands with `current_value`: `detection (loop=on, dup=off)`, `display (markdown=on)`, `toolfold (auto)`, etc.
- **On failure**: Empty response.

#### [NC-03] `/set detection` — list deeper subcommands

- **Given**: User types `/set detection` with no value
- **Input**: Enter
- **Expected**: Shows subcommands: `loop (off)`, `duplicate (off)`.
- **On failure**: Treated as setting key and produces error.

#### [NC-04] `/get detection` — show value at node

- **Given**: Tree: `get` → ArgSpec: optional `key`
- **Input**: `/get detection`
- **Expected**: Tree walk: `get` → remaining `"detection"` matched against `key` arg. Handler iterates all nodes whose name starts with `detection`, shows current values.
- **On failure**: No match for key.

#### [NC-05] `/set detection loop toggle` — toggle value

- **Given**: `loop` has Choice `on|off|toggle`
- **Input**: `/set detection loop toggle`
- **Expected**: `toggle` maps to "flip current state". Reads `cfg_.detection_loop`, flips it. Status shows new state.
- **On failure**: `toggle` treated as literal string value.

#### [NC-06] Unknown subcommand at depth 2

- **Given**: Tree: `set` → `detection` → (loop, duplicate)
- **Input**: `/set detection foo on`
- **Expected**: Tree walk: `set` → `detection` → `foo` not found → error: `"unknown command: /set detection foo"`. No dispatch.
- **On failure**: Silent no-op.

#### [NC-07] `/set compression threshold 0.75` — range validation

- **Given**: `threshold` has ArgSpec: Float range [0.1, 1.0]
- **Input**: `/set compression threshold 0.75`
- **Expected**: Validates `0.75` is within [0.1, 1.0] → sets `compression_threshold`. Status: `"compression threshold: 0.75"`.
- **On failure**: Value out of range → error: `"threshold must be between 0.1 and 1.0 (got: 2.0)"`.

#### [NC-08] `/set compression threshold 5` — type error

- **Given**: ArgSpec expects Float, got `5`
- **Input**: `/set compression threshold 5`
- **Expected**: `5` is parsed as valid float. OK (integer inputs parse as float). But if out of range, error.
- **On failure**: Type mismatch error with description.

#### [NC-09] `/set provider openrouter` — leaf with no subcommands

- **Given**: `provider` has ArgSpec: String (provider name)
- **Input**: `/set provider openrouter`
- **Expected**: Matches String arg → calls `cmd_provider("openrouter")`. Global config updated.
- **On failure**: Treated as unknown subcommand.

#### [NC-10] Deep command with Tab completion — one level per press

- **Given**: User types `/set det`
- **Input**: Tab
- **Expected**: Tree walk resolves to `set` level. Completion scans subcommands starting with `det` → matches `detection`. Input becomes `/set detection`.
- **Input**: Tab again
- **Expected**: No shadow extension (only one match). Input stays `/set detection` + adds trailing space.
- **On failure**: Completion jumps to `/set detection loop` (two levels in one press).

#### [NC-11] Key-chain mode: `/config network proxy http://proxy:8080`

- **Given**: `config` node has `allow_key_chain = true`, `min_keys = 1`, `value_spec` = ArgSpec(Url). No subcommands match `network` or `proxy`.
- **Input**: `/config network proxy http://proxy:8080`
- **Expected**: Tree walk: `config` → `network` not found in subcommands → key-chain fallback. Keys = `["network", "proxy"]`. Value = `"http://proxy:8080"`. Validates as URL. Handler invoked with `{key_chain: ["network","proxy"], value: "http://proxy:8080"}`. Config updated.
- **On failure**: `network` matches a subcommand by accident (should be caught at definition time).

#### [NC-12] Key-chain mode: single key-value

- **Given**: `config` has `allow_key_chain = true`, `min_keys = 0`
- **Input**: `/config theme dark`
- **Expected**: Keys = `[]` (empty). Value = `"dark"`. Matches `value_spec` (Choice: `dark`, `light`). Handler sets theme.
- **On failure**: `"dark"` must match the value spec; if not (e.g. `"blink"`), error: `"invalid value: blink (expected: dark|light)"`.

#### [NC-13] Key-chain mode: too few keys

- **Given**: `config` has `allow_key_chain = true`, `min_keys = 2`
- **Input**: `/config theme`
- **Expected**: Remaining tokens = `["theme"]`. `remaining.size()` (1) < `min_keys + 1` (3). Falls through to error: `"config requires at least 2 key tokens and a value"`.
- **On failure**: Key-chain resolution triggered with partial keys.

#### [NC-14] Key-chain mode: value type validation

- **Given**: `value_spec` = ArgSpec(Float, range [0.0, 1.0])
- **Input**: `/config opacity 0.75`
- **Expected**: Value `"0.75"` parsed as Float, within range. Handler invoked.
- **Input**: `/config opacity abc`
- **Expected**: Value parse fails. Error: `"opacity: expected a number (0.0-1.0), got 'abc'"`.

#### [NC-15] Mixed: fixed subcommand + key-chain on same node

- **Given**: `config` has subcommand `reset` (fixed) AND `allow_key_chain = true`
- **Input**: `/config reset`
- **Expected**: Subcommand `reset` matches → runs reset handler (Mode 1 wins).
- **Input**: `/config theme dark`
- **Expected**: `theme` not found in subcommands → key-chain fallback (Mode 2).
- **On failure**: Key-chain hides fixed subcommand, or fixed subcommand prevents key-chain.

#### [NC-16] Key-chain with `?` help

- **Given**: User types `/config `
- **Input**: `?`
- **Expected**: Popup shows fixed subcommands first, then key-chain hint: `"<key>... <value: string>  - Set arbitrary config keys"`.
- **Given**: User types `/config network proxy `
- **Input**: `?`
- **Expected**: Popup shows value spec: `"<value: url>  - Proxy URL (e.g., http://proxy:8080)"`.

#### [NC-17] Key-chain with completion

- **Given**: User types `/config ` + Tab
- **Expected**: Drawer shows fixed subcommands + key-chain hint as a completion source.
- **Given**: User types `/config ne` + Tab
- **Expected**: No fixed subcommand matches `ne`. Key-chain mode: token `ne` is a partial key. No value expected yet. Input not completed (waiting for more keys + value).

#### [NC-18] Arbitrary nesting depth

- **Given**: `config` node with `allow_key_chain = true`, `min_keys = 0`
- **Input**: `/config a b c d e f value`
- **Expected**: No subcommand match. Key-chain collects `["a","b","c","d","e","f"]`. Value = `"value"`. Handler receives arbitrary-depth key path. Works with 6 key tokens.
- **On failure**: Depth limit enforced or crash.

#### [NC-19] Key-chain with flags

- **Given**: `config` has flag `--json`
- **Input**: `/config --json network proxy http://proxy:8080`
- **Expected**: `--json` extracted as flag. Remaining = `["network","proxy","http://proxy:8080"]`. Key-chain resolves. Handler gets `{flags: {json: "true"}, key_chain: ["network","proxy"], value: "http://proxy:8080"}`.

#### [NC-20] Pattern: dot-separated keys as single token

- **Given**: User types `/config network.proxy.url http://proxy:8080`
- **Input**: Enter
- **Expected**: Key-chain can optionally split `.`-separated tokens into key parts: `["network", "proxy", "url"]`. Or treat the dot as part of the key (configurable).
- **Note**: This is a DESIRED feature choice point — not yet specified which behaviour wins.

---

#### [NC-21] `?` at each depth level

- **Given**: User types `/set `
- **Input**: `?`
- **Expected**: Popup shows subcommands of `set`: `detection`, `display`, `toolfold`, etc. with descriptions and current values.
- **Given**: User types `/set detection `
- **Input**: `?`
- **Expected**: Popup shows subcommands of `detection`: `loop`, `duplicate` with values.
- **Given**: User types `/set detection loop `
- **Input**: `?`
- **Expected**: Popup shows ArgSpec choices: `on`, `off`, `toggle`.

---

### Cross-references

- **Depends on**: `tui/input-system/slash-engine.md`, `tui/input-system/auto-complete.md`, `tui/input-system/contextual-help.md`
- **Depended on by**: `config/ui-config.md`
- **Reference implementation**: `completion/` library (standalone, compiled but unused by TUI)
- **Test coverage**: No direct TUI tests. `completion/tests/completion_test.cpp` tests the tree model.

### Known gaps

1. **Flat command table (current)** — TUI `Command` struct has no `subcommands` vector. Deep nesting is manual string parsing in handlers. Must migrate to `CommandNode` tree model with key-chain support.
2. **No ArgSpec or FlagSpec (current)** — All arguments are untyped strings. No type validation, range checks, or choice restriction at parse time.
3. **Standalone `completion/` library is the migration target** — It implements basic tree model and `ArgSpec`/`FlagSpec` but lacks key-chain mode. Would need `allow_key_chain`, `value_spec` additions.
4. **No key-chain mode (current)** — Truly arbitrary key-depth configs like `/config a/b/c value` require the key-chain fallback model.
5. **Dot-separated key splitting** — Whether `/config network.proxy.url` splits into three keys or one is undecided. Configurable per node.
6. **No flag support** — `--verbose`, `--help`, `--json` as command-line-style flags not supported.
7. **Key-chain completion** — In key-chain mode, Tab should complete partial keys against a live source (e.g., known config paths from the server). Not yet specified.
