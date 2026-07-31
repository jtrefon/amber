## Spec: MCP Client Session and Adapters

### Purpose

Define the per-server `MCPClient` (initialize, capability negotiation,
discovery, request plumbing) and the adapters that surface server primitives as
amber-native surfaces: tools → `ToolRegistry`, resources → `read_resource`,
prompts → the command tree. This spec is transport-agnostic; it sits on top of
`mcp/mcp-transport.md`.

### Ownership

- **Source files** (target): `include/agent/mcp_client.h`,
  `lib/mcp_client.cpp`, `lib/mcp_tools.cpp`, `lib/mcp_config.cpp`
  (`ServerManager` + config files)
- **Test files** (target): `tests/mcp_client_test.cpp`,
  `tests/mcp_tools_test.cpp`, `tests/mcp_config_test.cpp`
- **Spec status**: design — implementation tracked in `docs/mcp-tracker.md`
  (MC-IMP-001, MC-IMP-004, MC-IMP-005, MC-IMP-006, MC-IMP-007).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Server config; transport bytes; `tools/list` etc. results; user/model invocations |
| **Output** | Registered `Tool` adapters; `read_resource` results; prompt template data; `/mcp show` snapshots |
| **Error states** | Version mismatch, capability absence, pagination failure, unknown tool/resource/prompt, timeouts, disconnect |
| **Invariants** | See below. |
| **Thread safety** | One `MCPClient` per server; calls serialized on a mutex; discovery and tool calls on the agent thread; UI reads snapshots via `ServerManager::snapshot()`. |

### Configuration (server registry)

Per-server config file — `~/.config/amber/mcp/<name>.conf` (global) and
`.amber/mcp/<name>.conf` (project, wins on name collisions). Flat `KEY=VALUE`
like the providers dir:

```
type=stdio | http
command=/path/to/server            # stdio: executable (no shell)
args=--arg1 value1 --flag          # stdio: space-separated; no quoting in v1
cwd=                               # stdio: default = workspace root
url=https://host/mcp               # http
auth_token=                        # http: static bearer token (never logged)
enabled=1|0                        # default 1
auto_connect=1|0                   # default 0 (connect on demand)
trusted=0|1                        # default 0 — see mcp-security.md
timeout_s=60                       # per-request, default 60
```

Validation: `name` must be kebab-case; `type` required; stdio requires
`command`; http requires `url`. Invalid entries are listed by
`/mcp list` with the error and skipped — never fatal.

**ServerManager** (session-scoped, owned by the host like `JobService`):

- `connect(name)` / `disconnect(name)` / `refresh(name)` /
  `snapshot()` (name, state, capabilities, tool count, error) for the UI.
- Owns `MCPClient` instances; unregisters tool adapters on disconnect.
- At session start connects every `enabled && auto_connect` server; on exit
  disconnects all (see [MA-06]).

### Initialization and capabilities

`initialize` request carries: `protocolVersion="2025-06-18"`, client
capabilities **{}** (v1 declares no roots/sampling/elicitation), clientInfo
`{"name":"amber","version":<version.h>}`. The client then:

1. Negotiates the version (server's response version wins if supported, else
   disconnect with a typed error).
2. Records server capabilities: `tools` (with `listChanged`), `resources`,
   `prompts`, `logging`, `completions`.
3. Sends `notifications/initialized`.
4. Runs discovery for every declared capability; `listChanged` notifications
   re-run the corresponding discovery.

Pagination: `nextCursor` is honored with a 10-page hard cap (a server looping
cursors is declared broken and disconnected).

### Tool adapters

Built from `tools/list` results per the naming rules in
`mcp-architecture.md`:

- `parameters_schema()` ← `inputSchema` (passed through as-is; the registry
  adds name/description). `outputSchema` is stored but not enforced (v1).
- `description()` ← server description, **prefixed** with a trust marker when
  the server is untrusted: `"[untrusted server] <description>".` The marker
  survives into the tool advertising and approval summary.
- `execute(args)` → `tools/call`; the result maps:
  - `isError: true` → `ToolResult{ok=false, error=joined text}`
  - content blocks: text blocks joined; embedded resource text appended with a
    `[resource: <uri>]` note; image/audio blocks summarized as
    `[image <mime>, <n> bytes]` (v1 does not render binaries into context);
    `structuredContent` is serialized as JSON text when present.
  - output capped at 64 KiB; truncation appends `[truncated: <n> bytes]`.
- Approval and mode follow the trust policy in `mcp-security.md`.

#### [MP-01] Tool adapter registration and invocation

- **Given**: server `github` exposes `get_issue` with an `inputSchema`
- **Input**: connect + model call
- **Expected**: `registry.find("mcp_github_get_issue")` works; args validate
  against the schema; result text reaches the conversation.
- **On failure**: Schema mismatch or `isError` → typed `ToolResult` error.

#### [MP-02] Tool name sanitization and collisions

- **Given**: server exposes `my.tool` and another server exposes `my_tool`
- **Input**: discovery
- **Expected**: Both register (`mcp_a_my_tool`, `mcp_b_my_tool` or `_2` suffix);
  `/mcp show` documents the mapping; no silent overwrite.
- **On failure**: A tool silently replaces another.

### Resources

- Discovery: `resources/list` (paginated) → kept as a snapshot for
  `/mcp show <server> resources`.
- Access: the built-in `read_resource` tool, params `server` + `uri`:
  - Unknown server / uri → `ToolResult{ok=false, error="unknown resource: …"}`.
  - Response text capped at 256 KiB (truncation marker appended); blobs are
    rejected with a typed error (v1: text only).
  - The result is ordinary conversation text — advisory, untrusted.
- No subscriptions, no auto-fetch of resource links in tool results (v1).

#### [MP-03] read_resource with a text resource

- **Given**: server exposes `doc://architecture`
- **Input**: `read_resource(server="docs", uri="doc://architecture")`
- **Expected**: Text returned, capped; embedded in the conversation as data.
- **On failure**: Oversized or blob content → typed rejection, no partial dump.

### Prompts

- Discovery: `prompts/list` → cached per server.
- Invocation surface (user only):
  - `/prompt list` — all prompts across connected servers.
  - `/prompt <server> <name>` — `prompts/get` with no args (all optional).
  - `/prompt <server> <name> <k>=<v> ...` — arguments from the command line;
    required-argument validation happens client-side from the declared
    `arguments` list (missing required → usage error, no server round-trip).
  - The dynamic `/mcp <server>` subtree lists the server's prompts as
    subcommands (see `mcp-ui.md`).
- Result handling: `prompts/get` messages are flattened to text (role labels
  preserved for the user) and **fill the input line for editing**. Nothing is
  sent until Enter. Embedded resources in prompt messages are passed through
  as text with a `[resource: <uri>]` note.
- The model can never reach prompts: no tool adapter exists for them, and the
  system prompt states prompts are user-invoked.

#### [MP-04] Prompt with arguments

- **Given**: server exposes `review_pr` with required `pr_number`
- **Input**: `/prompt github review_pr pr_number=42`
- **Expected**: `prompts/get` called with `{"pr_number":"42"}`; returned text
  fills the input line; cursor at end.
- **On failure**: Missing required arg → usage error before any round-trip.

#### [MP-05] Unknown prompt

- **Given**: `/prompt github nope`
- **Input**: the command
- **Expected**: `error: unknown prompt 'nope' on server 'github'` (server
  round-trip; the server may have a dynamic list).
- **On failure**: Empty template silently inserted.

### Logging capability

Servers may emit `notifications/logging/message`. v1: these are size-capped
(64 KiB) and forwarded to the amber debug log with the server name prefix;
never rendered into the conversation.

### Completion capability

`completions/complete` (prompt-argument autocomplete) is **deferred**: the
argument values come from the command line only. Noted as a known gap.

---

### Invariants

1. **Discovery is explicit.** `tools/list` etc. run at connect and on
   `listChanged`/`/mcp refresh` only — never on every turn.
2. **One capability, one surface.** Tools → registry; resources →
   `read_resource`; prompts → user commands. No primitive leaks into another
   surface (a prompt never becomes a tool).
3. **Adapters are recreated on reconnect.** Tool state, caches, and discovery
   snapshots are dropped with the session; nothing is reused across connects.
4. **Names are stable and deterministic** so policy rules survive restarts.
5. **No capability, no surface.** A server that doesn't declare `tools` gets no
   tool adapters; amber never probes undeclared capabilities.

---

### Cross-references

- **Depends on**: `mcp/mcp-architecture.md`, `mcp/mcp-transport.md`, `mcp/mcp-security.md`, `docs/spec/agent-loop/tool-dispatch.md`, `docs/spec/config/file-config.md`
- **Depended on by**: `mcp/mcp-ui.md`, `docs/mcp-tracker.md`
- **Test coverage**: `tests/mcp_client_test.cpp`, `tests/mcp_tools_test.cpp`, `tests/mcp_config_test.cpp`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (client-only; no roots/sampling; no subscriptions; no completions) |
