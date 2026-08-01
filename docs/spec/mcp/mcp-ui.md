## Spec: MCP User Interface (Commands, Prompts, Panels)

### Purpose

Define how the user drives MCP from amber: the `/mcp` command tree, the
`/prompt` surface (MCP prompts as slash commands, including `get`/`set`-style
control), the server manager UI, and the status-bar footprint. The model's
half (tool adapters) is covered by `mcp/mcp-client.md`; this spec is the
user-controlled half.

### Ownership

- **Source files** (target): `tui/tui_input.cpp` (`cmd_mcp`, `cmd_prompt`),
  `tui/tui_session.cpp` (lifecycle wiring), `tui/tui_render.cpp` (status bar,
  server panel), `src/main.cpp` (CLI surface), `lib/mcp_config.cpp`
- **Test files** (target): `tests/mcp_config_test.cpp` (command backends),
  `tests/tui_tests.cpp` (command glue where Tui is not required)
- **Spec status**: design — implementation tracked in `docs/mcp-tracker.md`
  (MC-IMP-006, MC-IMP-008, MC-IMP-010).

---

### Command tree

`/mcp` — MCP server management. Follows the `CommandNode` model of
`nested-commands.md` (fixed subcommands; the `<server>` level resolves
dynamically from the server registry, and prompts appear as leaves).

```
mcp                                     — MCP management
  list                                  — Servers + state (enabled/connected/tools/prompts/error)
  show <server>                         — Capabilities, tools (name → mcp_<s>_<t>), resources, prompts
  connect <server>                      — Connect now (spawn/initialize/discover)
  disconnect <server>                   — Shut down the session
  refresh <server>                      — Re-run discovery (tools/resources/prompts)
  add <name> <type>                     — Create a server config (interactive prompts follow
                                          the provider-add pattern)
  edit <name>                           — Edit config (form dialog)
  delete <name>                         — Remove config; disconnect if running
  enable <name> | disable <name>        — Flip `enabled`; disable disconnects
  trust <name> on|off                   — Flip `trusted` (see mcp-security.md)
  prompts <server>                      — List that server's prompts
```

`/prompt` — MCP prompt templates (user-controlled, per the MCP spec):

```
prompt list                             — All prompts across connected servers
prompt <server> <name> [k=v ...]        — Get the template, fill the input line for editing
```

**`get`/`set`-style control (BitchX convention):** prompt state is visible and
mutable through the same dotted-key machinery as other state:

- `/get mcp.prompts` and `/get mcp.prompts.<server>` list prompts (mirrors
  `/get skills`).
- `/get mcp.servers`, `/get mcp.servers.<name>` show config + live state.
- `/set mcp.trust <server> on|off` is sugar for `/mcp trust …` and persists to
  the project config; `/set mcp.enable <server> on|off` persists `enabled`.
- The SettingRegistry gains a dynamic `mcp.*` subtree populated from the
  ServerManager snapshot, so `/get`, completion, and the `?` help path all
  work without special cases (see `slash-engine.md` invariants).

### Dynamic prompt subtree

- When a server connects, `prompts/list` results populate the
  `/mcp <server>` completion: typing `/mcp github ` shows the server's prompts
  as selectable leaves with their descriptions (plus `show|refresh|disconnect|…`).
  Selecting one invokes `/prompt github <name>`.
- Prompts never appear in the model-visible tools; they are a drawer/command
  surface only.

### Status bar

- When ≥1 server is connected: `mcp: <name>·<name>…` appended to the right
  side of the status bar (after the context gauge). A server in
  `error`/`disconnected` state shows `!<name>`.
- Tool-fold markers reuse the existing tool-result folding; MCP tool calls
  render exactly like built-in tool calls (name is `mcp_<server>_<tool>`).

### Server manager dialog

- `/mcp list` renders a table in the scrollback (name · type · state ·
  tools · prompts · error).
- `show <server>` renders: capabilities, negotiated version, tool mapping
  table (`mcp_github_get_issue ← get_issue`), resource URIs, prompt names.
- Errors from connect/discover are rendered inline as status lines with the
  server name prefix; they never block the input loop.

### CLI surface (headless)

- `amber --mcp-list` — print the server table (plain text; `--mcp-list --json`
  prints the snapshot as JSON for scripting).
- `amber --mcp <name> <prompt> [k=v ...]` — non-interactive prompt retrieval:
  prints the flattened template to stdout and exits. No model invocation.
- `amber --mcp-connect <name>` — connect at startup (overrides
  `auto_connect`).
- Tool adapters work in headless mode exactly as in the TUI (same registry);
  approval falls back to the CLI prompt/`--yes` policy.

#### [MU-01] /mcp list renders state

- **Given**: `github` connected, `db` configured but disabled, `browser`
  failed to spawn
- **Input**: `/mcp list`
- **Expected**: Three rows with correct states (`connected`, `disabled`,
  `disconnected`), tool/prompt counts, and the spawn error for `browser`.
- **On failure**: State table lies about connectivity.

#### [MU-02] Prompt fills the input line

- **Given**: `github` exposes `review_pr`
- **Input**: `/prompt github review_pr pr_number=42`
- **Expected**: Input line contains the template text (editable); nothing sent;
  Enter sends it as the user's message.
- **On failure**: Template sent without review (MCP prompt-injection vector).

#### [MU-03] /get mcp.prompts lists across servers

- **Given**: two connected servers with prompts
- **Input**: `/get mcp.prompts`
- **Expected**: `server · prompt · description` lines for both servers; a
  per-server subkey filters.
- **On failure**: Prompts hidden from the user.

#### [MU-04] /set mcp.trust persists

- **Given**: `github` untrusted
- **Input**: `/set mcp.trust github on`
- **Expected**: `trusted=1` written to `.amber/mcp/github.conf`; the approval
  gate for `mcp_github_*` tools switches off from the next call; status line
  confirms. `/no set mcp.trust github` reverts.
- **On failure**: Trust change lost on restart.

#### [MU-05] Server crash surfaces in the UI

- **Given**: `github` connected; its process exits
- **Input**: the next tool call
- **Expected**: Tool error mentions the disconnect; status bar flips to
  `!github`; `/mcp connect github` restarts.
- **On failure**: Silent failure with a stale `connected` indicator.

#### [MU-06] Headless prompt retrieval

- **Given**: `github` connected (auto_connect) in a CLI run
- **Input**: `amber --mcp github review_pr pr_number=42`
- **Expected**: Flattened template on stdout; process exits 0.
- **On failure**: Non-zero exit with a typed error; nothing partial printed.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Slash commands, `/get`/`/set` dotted keys, status-bar tick, CLI flags |
| **Output** | Scrollback tables, input-line fills, status indicators, config writes, stdout text |
| **Error states** | Unknown server/prompt; failed connect; invalid config; CLI misuse |
| **Thread safety** | All UI reads via `ServerManager::snapshot()`; no direct access to live client state from the render thread. |

---

### Cross-references

- **Depends on**: `mcp/mcp-client.md`, `mcp/mcp-security.md`, `docs/spec/tui/input-system/nested-commands.md`, `docs/spec/tui/input-system/slash-engine.md`, `docs/spec/tui/settings-ui.md`
- **Depended on by**: `docs/mcp-tracker.md`
- **Test coverage**: `tests/mcp_config_test.cpp`, `tests/tui_tests.cpp`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (user-controlled prompts; dynamic `/mcp` subtree; `mcp.*` get/set keys; CLI flags) |
