## Spec: MCP Client Architecture (Umbrella)

### Purpose

Make amber an **MCP client** (host) so the user can connect it to any server in
the Model Context Protocol ecosystem — GitHub, database, browser, or internal
tooling — without amber implementing each integration. MCP servers expose three
primitives: **tools** (model-executable functions), **resources** (readable
context), and **prompts** (user-invoked templates). This spec defines the
umbrella contract: scope, terminology, primitive mapping, session lifecycle,
configuration, and security posture. Wire details live in
`mcp/mcp-transport.md`; the client session and adapters live in
`mcp/mcp-client.md`; the user interface lives in `mcp/mcp-ui.md`; the trust
model lives in `mcp/mcp-security.md`.

This is a deliberate revisit of the MISSION.md feature filter, which previously
rejected MCP ("Standardise on tool API, not another protocol"). The internal
abstraction stays the `Tool` port; MCP becomes one more **adapter** over it
(hexagonal, same as `SkillCatalog` and the search backends). The rationale that
"a plugin loader is not wanted" (MISSION.md, Plugin system row) is untouched:
MCP is a *protocol adapter for third-party capability servers*, not a plugin
loader for amber itself. See `MISSION.md` revision below.

### Ownership

- **Source files** (target): `include/agent/mcp_client.h` (session),
  `include/agent/mcp_transport.h` (transport port), `lib/mcp_jsonrpc.cpp`,
  `lib/mcp_transport_stdio.cpp`, `lib/mcp_transport_http.cpp`,
  `lib/mcp_client.cpp`, `lib/mcp_tools.cpp` (tool/resource adapters),
  `lib/mcp_config.cpp` (server registry + config), TUI wiring in
  `tui/tui_input.cpp` (prompt + `/mcp` commands), `tui/tui_session.cpp`,
  `src/main.cpp` (CLI surface)
- **Test files** (target): `tests/mcp_transport_test.cpp`,
  `tests/mcp_client_test.cpp`, `tests/mcp_tools_test.cpp`,
  `tests/mcp_config_test.cpp` (all added to `UNITTEST_OBJ` in `Makefile.in`),
  `tests/tui_tests.cpp` (command glue where feasible)
- **Spec status**: design — implementation tracked in `docs/mcp-tracker.md`.

---

### Scope decision

| In scope (v1) | Out of scope (v1, documented) |
|---|---|
| amber as **MCP client** over **stdio** and **Streamable HTTP** transports | amber as an MCP **server** (exposing amber tools to other hosts) |
| Server **tools** → `Tool` adapters through the existing approval/read-mode gates | Client **roots** capability (never expose the filesystem to servers) |
| Server **resources** → `read_resource` tool, embedded-resource passthrough | Client **sampling** and **elicitation** (server-initiated LLM calls / questions) |
| Server **prompts** → `/prompt` command + dynamic `/mcp <server>` subtree | OAuth 2.1 discovery flows for remote servers (static bearer token only) |
| Per-server trust levels, output caps, timeouts, cancellation, audit logging | Resource subscriptions (`resources/subscribe`); `completions` endpoint (deferred) |
| 2025-06-18 protocol version, negotiated at initialize | 2024-11-05 HTTP+SSE legacy transport (tolerate via version negotiation errors) |

The scope boundary follows the MCP security principles: servers are untrusted
until the user opts in, every side effect crosses an approval gate, and amber
never hands a server access to its own tools, workspace, or model.

---

### Architecture

```
                ┌────────────────────────────────────────────────────┐
                │                    ServerManager                    │
                │  config (global + project, name-keyed)              │
                │  lifecycle: connect on demand / auto_connect,       │
                │  shutdown on exit (SIGTERM→SIGKILL, HTTP DELETE)    │
                └──────────────┬─────────────────────────────────────┘
                               │ owns
                ┌──────────────▼─────────────────────────────────────┐
                │                    MCPClient (per server)           │
                │  initialize + version/capability negotiation        │
                │  primitives: tools[] · resources[] · prompts[]      │
                │  request/response correlation, timeouts, cancel     │
                └──────┬───────────────────────┬─────────────────────┘
                       │                       │
        ┌──────────────▼──────┐   ┌────────────▼──────────────┐
        │ StdioTransport      │   │ HttpTransport            │
        │ fork/exec, 3 pipes, │   │ POST per message, SSE,   │
        │ newline JSON-RPC,   │   │ Mcp-Session-Id, version  │
        │ stderr → log        │   │ header, curl + sse_parser│
        └──────────────┬──────┘   └────────────┬──────────────┘
                       └───────────────────────┘
                                    │
                    ┌───────────────▼───────────────────────────────┐
                    │                Adapters (in amber)             │
                    │  mcp_<server>_<tool> → Tool (approval-gated)   │
                    │  read_resource → Tool (capped, server-scoped)  │
                    │  prompts → /prompt + /mcp <server> subtree     │
                    └────────────────────────────────────────────────┘
```

### Terminology

| Term | Meaning |
|------|---------|
| **MCP server** | A process (stdio) or endpoint (Streamable HTTP) exposing tools/resources/prompts over JSON-RPC 2.0. |
| **MCP client / host** | Amber. Connects, negotiates, adapts primitives into amber surfaces. |
| **Server name** | Kebab-case identifier the user assigns (`github`, `db`, `browser`). The namespacing root for tools, commands, and config. |
| **Trust level** | Per-server config: `trusted=false` (default) or `trusted=true`. Controls the approval gate and read-mode policy. |
| **Tool adapter** | A runtime `Tool` built from a server's `tools/list` result. Name `mcp_<server>_<tool>`. |
| **Primitive** | One of tools / resources / prompts, per the MCP spec. |
| **Prompt** | A server-side message template invoked by the user (never the model) through `/prompt`. |
| **Embedded resource** | A resource block inside a tool result or prompt message; its `text` is passed through, its URI is not auto-fetched (v1). |

---

### Primitive mapping

| MCP primitive | Amber surface | Control | Notes |
|---|---|---|---|
| `tools/list` + `tools/call` | Dynamic `Tool` registered at connect/refresh: `mcp_<server>_<tool>` | Model (via agent loop) | Approval gate per server trust; output capped (64 KiB); errors → `ToolResult{ok=false}` |
| `resources/list` + `resources/read` | Built-in `read_resource` tool (`server`, `uri` params) + `/mcp show <server> resources` | User and model | Output capped (256 KiB); no auto-subscription |
| Embedded resources in results/prompts | Passed through as text; URI rendered as a note | — | Never auto-fetched in v1 |
| `prompts/list` + `prompts/get` | `/prompt <server> <name> [k=v …]` + dynamic entries in the `/mcp <server>` subtree | User only | `prompts/get` fills the input line (editable) — the model never triggers prompts |

**Tool naming rules:**

1. Adapter name = `mcp_` + sanitized server name + `_` + sanitized tool name.
2. Sanitization: non-`[a-zA-Z0-9_-]` → `_`; result truncated to 48 chars; the
   full namespaced name must fit the 64-char OpenAI tool-name limit or the tool
   is excluded with a warning.
3. Name collisions in the registry (two servers expose the same tool name after
   sanitization, or a collision with a built-in) are resolved deterministically:
   first registration wins; later ones get a numeric suffix (`_2`) and a
   warning. The discovery block and `/mcp show` surface the mapping.
4. `is_read_only()` is always `false` (annotations are untrusted — see
   `mcp-security.md`). `requires_approval()` is `true` unless the server is
   configured `trusted=true`.
5. `summarize()` returns `mcp <server>: <tool>(<n args>)` for approval prompts.

---

### Session lifecycle

| Phase | What happens |
|-------|--------------|
| **Config** | Servers are declared in `~/.config/amber/mcp/<name>.conf` (global) and `.amber/mcp/<name>.conf` (project, wins). `enabled=false` skips; `auto_connect=true` connects at session start. |
| **Connect** | On session start (auto_connect) or `/mcp connect <name>`. Stdio: spawn, `initialize` (10 s timeout), `notifications/initialized`. HTTP: POST initialize with session-id capture. |
| **Discover** | `tools/list`, `resources/list`, `prompts/list` (paginated). Tool adapters are registered into the shared `ToolRegistry`; prompts populate the `/mcp <server>` subtree; listChanged notifications trigger re-discovery. |
| **Operate** | Tools run through the normal dispatch path (approval, mode, loop detection, log). `read_resource` is a normal tool. `listChanged` re-runs discovery without disconnecting. |
| **Disconnect** | `/mcp disconnect <name>` or session end. Stdio: close stdin → SIGTERM (3 s) → SIGKILL. HTTP: `DELETE` with `Mcp-Session-Id`. Adapters unregistered; server state dropped. |
| **Crash** | Transport error or process exit → all in-flight calls fail with a typed error; the server is marked `disconnected`; `/mcp connect` restarts it. Amber never auto-restarts mid-session. |

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | MCP server config; user commands (`/mcp`, `/prompt`); model tool calls against adapters; server JSON-RPC traffic |
| **Output** | Tool results in the conversation; resource text; prompt templates in the input line; `/mcp show` tables; status-bar indicators |
| **Error states** | Config parse error; spawn failure; initialize timeout/version mismatch; request timeout; transport disconnect; tool execution error (`isError`); unknown server/prompt/resource |
| **Invariants** | See below. |
| **Thread safety** | Per-server client: one mutex; requests serialized per server; responses correlated by JSON-RPC id; the agent thread is the only caller of tool adapters; UI reads snapshots. |

### Invariants

1. **Every MCP primitive crosses an amber gate.** Tools: approval + mode.
   Resources: cap + explicit fetch. Prompts: user-initiated only.
2. **Servers never gain amber privileges.** No roots capability, no sampling,
   no workspace access, no access to amber's own tools. A server's only
   leverage is what its own tool results say — advisory text.
3. **Server input is untrusted.** Tool descriptions, annotations, resource and
   prompt text, and instructions may be malicious. They are rendered and
   injected as *data*, never as policy (see `mcp-security.md` [MS-01]–[MS-06]).
4. **The catalog/skill pipeline is unchanged.** MCP tools are ordinary `Tool`s;
   discovery, precedence, and the context hash chain are untouched.
5. **Bounded everything.** Tool results ≤ 64 KiB, resources ≤ 256 KiB, prompt
   text ≤ 32 KiB, per-request timeout (default 60 s), initialization ≤ 10 s.
   Exceeding fails closed (error), never silently overflows.
6. **Deterministic names.** `mcp_<server>_<tool>` is stable across connects, so
   `/set policy rule` entries survive restarts.
7. **No auto-restart.** A crashed server stays down until the user reconnects.
8. **Lifecycle cleanup.** On exit every stdio server is SIGTERM'd then SIGKILL'd;
   HTTP sessions get `DELETE`. No orphaned processes.

---

### Scenarios

#### [MA-01] Session start connects enabled servers

- **Given**: config declares `github` (stdio, auto_connect) and `db` (http, auto_connect); `browser` (stdio, auto_connect=false)
- **Input**: amber starts a session
- **Expected**: `github` and `db` connect, initialize, and register their tools; `browser` stays dormant. Status bar shows `mcp: github · db`.
- **On failure**: One server failing to spawn/initialize does not block the session; the server is listed as `disconnected` with the error; other servers proceed.

#### [MA-02] Model calls an MCP tool through the approval gate

- **Given**: `github` (trusted=false) exposes `create_issue`; adapter `mcp_github_create_issue` registered
- **Input**: Model calls `mcp_github_create_issue`
- **Expected**: Approval prompt shows `mcp github: create_issue(3 args)`; on approve the call runs; result returns as a normal `ToolResult`. On deny, `denied by user` (same path as bash).
- **On failure**: Approval bypassed for an untrusted server.

#### [MA-03] listChanged refreshes without reconnecting

- **Given**: `github` connected; server emits `notifications/tools/list_changed`
- **Input**: Notification received
- **Expected**: `tools/list` re-runs; new tools register, removed tools unregister; session stays connected.
- **On failure**: Stale tool list advertised to the model.

#### [MA-04] Server crash mid-session

- **Given**: `github` connected; model calls `mcp_github_get_issue`
- **Input**: The server process exits while the call is in flight
- **Expected**: The call fails with `ToolResult{ok=false, error="mcp server github disconnected"}`; server marked disconnected; `/mcp connect github` restarts it.
- **On failure**: Amber hangs on the call or auto-restarts the server.

#### [MA-05] Prompt invoked by the user only

- **Given**: `github` exposes prompt `review_pr` (arguments: `pr_number` required)
- **Input**: User types `/prompt github review_pr pr_number=42`
- **Expected**: `prompts/get` returns messages; the assembled text fills the input line for editing; nothing is sent until the user presses Enter. A model tool call cannot reach prompts.
- **On failure**: Prompt template injected into context without user consent.

#### [MA-06] Disconnect on exit

- **Given**: Two stdio servers + one HTTP server connected
- **Input**: Session ends (TUI quit / CLI exit)
- **Expected**: Stdio servers: stdin closed, SIGTERM, then SIGKILL after 3 s. HTTP: DELETE with session id. `ps` shows no orphaned MCP server processes.
- **On failure**: Zombie server processes survive amber.

---

### Cross-references

- **Depends on**: `mcp/mcp-transport.md`, `mcp/mcp-client.md`, `mcp/mcp-ui.md`, `mcp/mcp-security.md`, `docs/spec/MISSION.md` (feature filter revision), `docs/spec/tui/input-system/nested-commands.md` (command tree), `docs/spec/agent-loop/tool-dispatch.md` (approval path), `docs/spec/workspace/security-model.md`
- **Depended on by**: `docs/spec/INDEX.md`, `docs/mcp-tracker.md`
- **Test coverage**: `tests/mcp_client_test.cpp`, `tests/mcp_transport_test.cpp`, `tests/mcp_tools_test.cpp`, `tests/mcp_config_test.cpp`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (client-only v1; stdio + Streamable HTTP; tools/resources/prompts adapters) |
