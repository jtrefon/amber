# amber — MCP Client Implementation Tracker

- **Status:** 🟡 In progress (design done, implementation pending sign-off)
- **Reference:** `docs/spec/mcp/mcp-architecture.md`, `mcp-transport.md`,
  `mcp-client.md`, `mcp-ui.md`, `mcp-security.md`
- **Issues register:** `docs/issues.md`

---

## How to Use This Tracker

1. Every task follows the **Red → Proposal → Sign-off → Green → PR** workflow
   (see AGENTS.md). On a branch named `<type>/mcp-<short-description>`:
   - **Red**: Write a failing test first (scenario IDs below map to the spec),
     commit it so CI shows the failure.
   - **Proposal**: Link the task below in the PR description.
   - **Sign-off**: Reviewer approves the proposed architecture.
   - **Green**: Implement; make the test pass; refactor to zero debt
     (classes ≤200 lines, methods ≤10 lines, SOLID, hexagonal boundaries).
   - **PR**: Open/update. All checks must pass.
2. Each task is **self-contained** and ordered by dependency (prerequisites first).
3. **Verification** must pass before marking a task `[done]`:
   `make clean && make && make test && make lint && make analyze`.
   If headers change, `make clean && make` regenerates the `.d` files.
4. No comments that restate code; first line of new files is functional
   (no SPDX/copyright boilerplate). Run `clang-format -i` on touched files.
5. Spec scenarios are the acceptance contract — the test names below reference
   the scenario IDs (e.g. `[MT-01]`), and the tracker row links them.

## Legend

```
[ ]     — Not started, ready for assignment
[~]     — Assigned and actively being worked
[x]     — Code merged, all checks pass, no known regressions
[!]     — Blocked on another task or external dependency
```

---

## Task 1: JSON-RPC wire layer (MC-IMP-001)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-001` |
| **Severity** | 🟠 High |
| **Depends on** | None (pure wire types) |
| **Blocks** | MC-IMP-002, MC-IMP-003 (both transports need it) |
| **Estimated effort** | 3-4 hours |
| **Files touched** | `include/agent/mcp_transport.h` (new: `McpMessage`, `McpError`, `McpRequest`), `lib/mcp_jsonrpc.cpp` (new), `tests/mcp_transport_test.cpp` (new, add to `UNITTEST_OBJ` in `Makefile.in`) |
| **Spec refs** | `mcp/mcp-transport.md` (envelope, error table), `mcp/mcp-client.md` [MP-01] |

### Problem

No JSON-RPC 2.0 wire types exist. Transports need a shared, testable
encode/decode layer with id correlation and the JSON-RPC error table.

### Target Architecture

- `McpMessage` (variant: request/notification/response) with `id`, `method`,
  `params`, `result`, `error`.
- `encode_msg(msg) -> std::string` — compact JSON, single line, no embedded
  newlines (stdio framing invariant).
- `decode_line(line) -> optional<McpMessage>` — tolerant parse: unknown fields
  ignored, invalid JSON → `McpError{-32700}`.
- `McpError{ code, message, data }` with the standard table
  (-32700/-32600/-32601/-32602/-32603/-32000..-32099) and `to_tool_error()`.
- Pure functions, no I/O — fully unit-testable.

### Refactor Rules

- No JSON-RPC knowledge outside this TU (and the transport shims).
- IDs are integers; a per-client counter lives in the client, not the wire layer.

### Verification

- [ ] Tests [MT-01] framing round-trip, error mapping, unknown-field tolerance,
      embedded-newline rejection at the encode step
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 2: stdio transport (MC-IMP-002)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-002` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-001 (wire), `lib/process.cpp` (spawn) |
| **Blocks** | MC-IMP-004 (client session) |
| **Estimated effort** | 5-7 hours |
| **Files touched** | `lib/process.cpp`/`include/agent/process.h` (`spawn_mcp_server`: fork/exec, 3 pipes), `lib/mcp_transport_stdio.cpp` (new), `tests/mcp_transport_test.cpp`, `tests/fixtures/mcp_echo.py` / `mcp_ignore_sigterm.py` (new fixtures) |
| **Spec refs** | `mcp/mcp-transport.md` [MT-01]–[MT-03] |

### Problem

`spawn_shell` fuses stdout/stderr and has no stdin pipe — unusable for MCP
stdio, which needs three separate pipes, direct exec (no shell), and
SIGTERM→SIGKILL shutdown.

### Target Architecture

- `spawn_mcp_server(command, args, cwd, stdin_fd, stdout_fd, stderr_fd, err)`
  in `lib/process.cpp` (fork, `execvp`, no shell, process-group isolation).
- `StdioTransport : McpTransport`:
  - writer: one line per message to child stdin (blocking with timeout).
  - reader thread: drains stdout → queue; stderr drained separately, capped
    64 KiB, forwarded to the debug log.
  - `send()` writes + waits on the response queue (id-keyed) with the request
    timeout; `request()`/`notify()`/`respond()` API.
  - `shutdown()`: close stdin → SIGTERM (3 s) → SIGKILL → reap (the MT-02
    escalation ladder).
- Fixture servers are real subprocesses (Python, stdlib only).

### Refactor Rules

- The transport owns its threads and queues; no globals.
- Child lifecycle is RAII (`McpTransport` destructor = shutdown).

### Verification

- [ ] Tests [MT-01] echo round-trip, [MT-02] SIGTERM-ignoring server escalates
      to SIGKILL (no zombie), [MT-03] stderr never corrupts the protocol
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 3: Streamable HTTP transport (MC-IMP-003)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-003` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-001 (wire), existing `libcurl`/`sse_parser` |
| **Blocks** | MC-IMP-004 (client session) |
| **Estimated effort** | 5-7 hours |
| **Files touched** | `lib/mcp_transport_http.cpp` (new), `lib/sse_parser.cpp` (reuse), `tests/mcp_transport_test.cpp`, `tests/fixtures/mcp_http_server.py` (new) |
| **Spec refs** | `mcp/mcp-transport.md` [MT-04]–[MT-06] |

### Problem

MCP Streamable HTTP is a different beast from the OpenAI chat endpoint:
per-message POST, SSE responses, session-id and protocol-version headers, and
404-triggered re-initialization.

### Target Architecture

- `HttpTransport : McpTransport`:
  - `send()` POSTs the message with `Accept: application/json,
    text/event-stream`, `MCP-Protocol-Version`, `Mcp-Session-Id` (when known),
    `Authorization: Bearer` (when configured).
  - `application/json` reply → parse directly. `text/event-stream` → drain SSE
    via `sse_parser` until the response for our id arrives; interleaved server
    requests/notifications are queued to the client.
  - Session id captured from the `Mcp-Session-Id` header on the initialize
    response; `delete_session()` sends `DELETE`.
  - HTTP 404 on `Mcp-Session-Id` → re-initialize once and retry the request
    once (MT-06).
  - Timeouts via curl `CURLOPT_TIMEOUT_MS` + the request-timeout clock.
- No `CURLOPT_SSL_VERIFYPEER` relaxation, ever.

### Refactor Rules

- curl usage stays inside this TU (and `http_transport.h` helpers); no curl
  types leak into the client.

### Verification

- [ ] Tests [MT-04] JSON round-trip, [MT-05] SSE multi-event response, [MT-06]
      stale-session 404 re-initializes once
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 4: MCPClient session (MC-IMP-004)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-004` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-002, MC-IMP-003 (transports) |
| **Blocks** | MC-IMP-005, MC-IMP-006 (adapters) |
| **Estimated effort** | 5-8 hours |
| **Files touched** | `include/agent/mcp_client.h` (new), `lib/mcp_client.cpp` (new), `tests/mcp_client_test.cpp` (new) |
| **Spec refs** | `mcp/mcp-client.md` (initialize, capabilities, pagination, listChanged), [MP-01]–[MP-05] |

### Problem

No session state: initialize/negotiation, capability recording, discovery with
pagination, and listChanged handling are all missing.

### Target Architecture

- `MCPClient` (one per server):
  - `connect()`: transport up → `initialize` (10 s) → version negotiation →
    `notifications/initialized` → discovery.
  - Capability struct: `has_tools`, `has_resources`, `has_prompts`,
    `tools_list_changed`, `resources_list_changed`, `prompts_list_changed`,
    `has_logging`, `has_completions`.
  - Discovery: `tools/list`, `resources/list`, `prompts/list` — all
    paginated with a 10-page hard cap; results cached for the UI snapshot.
  - `notifications/tools|resources|prompts/list_changed` → re-run the
    matching discovery (no reconnect).
  - `call_tool(name, args) -> McpResult`, `read_resource(uri) -> McpResult`,
    `get_prompt(name, args) -> vector<McpPromptMessage>`, all with the request
    timeout and cancellation mapping.
  - `disconnect()`: transport shutdown; state dropped.
  - `snapshot()` for the UI.
- Client capabilities sent at initialize: `{}` (no roots/sampling/elicitation).

### Refactor Rules

- The client depends on the `McpTransport` port (DIP), not on either
  transport. A fake transport in tests drives all scenarios without subprocesses.
- No transport types leak past the client boundary.

### Verification

- [ ] Tests [MP-01]–[MP-05] via a fake transport; pagination cap; listChanged
      refresh; version-mismatch disconnect; sampling request → `-32601`
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 5: ServerManager + config (MC-IMP-005)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-005` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-004 (client) |
| **Blocks** | MC-IMP-006, MC-IMP-008 (UI) |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `lib/mcp_config.cpp` (new: config files, validation, `ServerManager`), `include/agent/mcp_config.h` (new), `lib/config.cpp` (env var `AMBER_MCP_SERVERS`), `tests/mcp_config_test.cpp` (new) |
| **Spec refs** | `mcp/mcp-client.md` (config section), `mcp/mcp-ui.md` [MU-04] |

### Problem

No place for servers to live: config schema, global+project precedence,
validation, and the session-scoped manager are all missing.

### Target Architecture

- Config file per server: `~/.config/amber/mcp/<name>.conf` (global) +
  `.amber/mcp/<name>.conf` (project wins). Flat `KEY=VALUE` keys as specified
  in `mcp-client.md` (type/command/args/cwd/url/auth_token/enabled/
  auto_connect/trusted/timeout_s).
- `load_mcp_servers()` → ordered map; invalid entries collected with errors
  (never fatal); token-bearing files written `0600`.
- `ServerManager`:
  - `connect/disconnect/refresh(name)`, `list()`, `snapshot()`.
  - Owns `unique_ptr<MCPClient>` per server + the registry hook for adapters
    (MC-IMP-006 fills it).
  - Auto-connect at session start; disconnect-all on exit.
  - `apply_trust(name, bool)` + `set_enabled(name, bool)` persist to the
    project config.
- Env `AMBER_MCP_SERVERS="name1,name2"` overrides `enabled` for a run.

### Refactor Rules

- Config parsing is a pure function (`parse_server_conf`) — unit-testable.
- The manager holds no UI types.

### Verification

- [ ] Tests: config precedence (project wins), validation errors surfaced not
      fatal, trust/enabled persistence, `0600` permissions, env override
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 6: Tool adapters + read_resource (MC-IMP-006)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-006` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-004, MC-IMP-005 |
| **Blocks** | MC-IMP-007 (trust wiring), MC-IMP-008 (UI) |
| **Estimated effort** | 5-8 hours |
| **Files touched** | `lib/mcp_tools.cpp` (new: `McpToolAdapter`, `ReadResourceTool`, registration), `lib/agent.cpp` (register adapters with the session registry), `tests/mcp_tools_test.cpp` (new) |
| **Spec refs** | `mcp/mcp-client.md` [MP-01]/[MP-02]/[MP-03], `mcp/mcp-architecture.md` (naming rules), `mcp/mcp-security.md` [MS-01]/[MS-02] |

### Problem

Server tools are not `Tool`s, so the model cannot reach them through the
normal dispatch/approval/log path.

### Target Architecture

- `McpToolAdapter : Tool` built from a `tools/list` entry + server context:
  - name `mcp_<server>_<tool>` (sanitized, collision-suffixed), schema passthrough.
  - `is_read_only()` → `false` always; `requires_approval()` →
    `!server.trusted`; `summarize()` → `mcp <server>: <tool>(<n>)`.
  - description prefixed `[untrusted server]` when not trusted.
  - `execute()` → `tools/call`; maps content blocks (text/embedded
    resource/image-note/structuredContent) and `isError`; 64 KiB cap.
- `ReadResourceTool : Tool` (read-only): `server` + `uri` params; 256 KiB cap;
  text-only; unknown server/uri → typed error.
- Registration: adapters join the shared `ToolRegistry` at connect/refresh,
  unregister on disconnect. Integration point: the session's registry (CLI
  and TUI share it; the skills tools pattern in `lib/agent.cpp` is the model).
- The tool list must never exceed the LLM tool-array size; a per-server cap
  (default 100 tools, config `max_tools`) trims with a warning.

### Refactor Rules

- Adapters hold `MCPClient&` + server name; they are thin — no JSON-RPC here.
- The cap/truncation logic is a pure helper (`flatten_content`) — unit-testable.

### Verification

- [ ] Tests [MP-01]/[MP-02]/[MP-03], [MS-01]/[MS-02] (approval + read-only
      claims ignored), content flattening incl. truncation, registry
      registration/deregistration
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 7: Trust, caps, cancellation hardening (MC-IMP-007)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-007` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-006 (adapters) |
| **Blocks** | MC-IMP-009 (prompt note + CLI) |
| **Estimated effort** | 3-5 hours |
| **Files touched** | `lib/mcp_client.cpp` (cancellation, sampling rejection), `lib/mcp_tools.cpp` (caps), `tests/mcp_tools_test.cpp`, `tests/mcp_client_test.cpp` |
| **Spec refs** | `mcp/mcp-security.md` [MS-03]–[MS-08] |

### Problem

The hard guarantees (caps, cancellation, method rejection, token redaction)
must be proven by tests, not assumed.

### Target Architecture

- Cancellation: `CancellationToken` → `notifications/cancelled`; late
  responses discarded; hung-call timeout wiring end-to-end.
- Sampling/elicitation/roots requests from servers → answered `-32601`; test
  asserts the fake server receives it.
- Caps enforced at the adapter and the client (`read_resource`).
- `auth_token` redaction helper (used by config dump and `/mcp show`).
- System-prompt trust note (see MC-IMP-009) is the user-facing half.

### Refactor Rules

- No test hits a live network; all scenarios run against the fake transport
  and fixture subprocesses.

### Verification

- [ ] Tests [MS-03]–[MS-08]; `grep -rn 'auth_token' lib/` shows config-only
      reads and redacted output, never logs
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 8: /mcp + /prompt commands + mcp.* get/set (MC-IMP-008)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-008` |
| **Severity** | 🟠 High |
| **Depends on** | MC-IMP-005, MC-IMP-006 |
| **Blocks** | MC-IMP-010 (UI polish) |
| **Estimated effort** | 5-8 hours |
| **Files touched** | `tui/tui_input.cpp` (`cmd_mcp`, `cmd_prompt`, `/get`/`/set` wiring), `tui/setting_registry.cpp` (dynamic `mcp.*` subtree), `tests/mcp_config_test.cpp` (command backends), `tests/tui_tests.cpp` |
| **Spec refs** | `mcp/mcp-ui.md` [MU-01]–[MU-05] |

### Problem

No user-facing surface: servers can't be listed/connected/managed, prompts
can't be invoked, and state isn't reachable via `/get`/`/set`.

### Target Architecture

- `cmd_mcp` (list/show/connect/disconnect/refresh/add/edit/delete/
  enable/disable/trust/prompts) and `cmd_prompt` (list + `<server> <name>
  [k=v...]`), following the existing string-based command pattern until the
  `CommandNode` tree lands (`nested-commands.md`).
- `prompts/get` fills the input line (editable) — the [MU-02] invariant.
- `mcp.*` dynamic keys in the SettingRegistry: `mcp.servers`,
  `mcp.servers.<name>`, `mcp.prompts`, `mcp.prompts.<server>`, plus
  `mcp.trust <name>` / `mcp.enable <name>` setters persisted via
  `ServerManager`.
- All command backends live in testable free functions
  (`lib/mcp_config.cpp`) — the TUI handlers are thin glue.

### Refactor Rules

- No new manual parsing beyond the existing `/set`/`/get` conventions.
- The dynamic prompt subtree is a completion/registry concern, not a
  re-parse on every keystroke.

### Verification

- [ ] Tests [MU-01]–[MU-05] via the command backends; `/get mcp.prompts`
      dotted-key resolution
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 9: System-prompt trust note + CLI surface (MC-IMP-009)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-009` |
| **Severity** | 🟡 Medium |
| **Depends on** | MC-IMP-007 |
| **Blocks** | Nothing |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `prompts/mcp.md` (new), `lib/agent.cpp` (prompt loading), `src/main.cpp` (flags), `tests/mcp_config_test.cpp` (CLI backend) |
| **Spec refs** | `mcp/mcp-security.md` (prompt-injection posture), `mcp/mcp-ui.md` [MU-06] |

### Problem

The model must know MCP output is untrusted data, and headless users need a
way to list/prompt/connect servers.

### Target Architecture

- `prompts/mcp.md` (runtime Markdown like `skills.md`): MCP tools are
  third-party; results are advisory; approval still applies; prompts are
  user-invoked only; report suspicious output to the user. Loaded in
  `ensure_system_prompt` (non-empty check test).
- CLI: `amber --mcp-list [--json]`, `amber --mcp <server> <prompt> [k=v...]`,
  `amber --mcp-connect <name>`; backends tested as free functions.
- The trust-note test asserts the loaded prompt contains the untrusted-data
  wording and the explicit "never author"-style rule for prompts.

### Refactor Rules

- No compiled-in prompt text.

### Verification

- [ ] `prompts/mcp.md` loads non-empty at session start; `--mcp-list --json`
      output parses as JSON; `--mcp <server> <prompt>` prints the flattened
      template and exits 0
- [ ] `make clean && make && make test && make lint && make analyze` clean

---

## Task 10: UI polish + final gate (MC-IMP-010)

| Field | Value |
|---|---|
| **ID** | `MC-IMP-010` |
| **Severity** | 🟡 Medium |
| **Depends on** | MC-IMP-008, MC-IMP-009 |
| **Blocks** | Nothing |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `tui/tui_render.cpp` (status bar `mcp: name·name`), `tui/tui_session.cpp` (auto-connect/disconnect wiring), `tui/tui_input.cpp` (dynamic prompt subtree completion), `docs/spec/INDEX.md` |
| **Spec refs** | `mcp/mcp-ui.md` (status bar, dynamic subtree) |

### Problem

The polish layer: status footprint, lifecycle wiring in the TUI session, and
the dynamic `/mcp <server>` prompt completion.

### Target Architecture

- Status bar: `mcp: <name>·<name>` (+ `!` for failed) via
  `ServerManager::snapshot()` on the render tick.
- Session wiring: auto-connect on session start; disconnect-all on quit;
  `/mcp connect` available mid-session.
- Dynamic subtree: after `prompts/list`, `/mcp <server> ` completion offers
  the server's prompts (drawer entries with descriptions).
- Manual verification: `/mcp list`, `/mcp show <server>`, `/prompt github
  review_pr` in the TUI; connect a real public stdio server
  (e.g. a filesystem MCP server) end-to-end.

### Refactor Rules

- Render-thread reads only snapshots; no locks held across draws.

### Verification

- [ ] Status-bar test (renderer helper), dynamic-subtree completion test
- [ ] Manual: end-to-end with a real MCP server (stdio + http), approval
      flow, prompt fill, disconnect cleanliness (`ps` shows no orphans)
- [ ] Final: `make clean && make && make test && make lint && make analyze`
      clean; update `docs/spec/INDEX.md`; close tracker

---

## Dependency Graph

```
MC-IMP-001 (wire)
   ├── MC-IMP-002 (stdio) ─┐
   └── MC-IMP-003 (http)  ─┴── MC-IMP-004 (session) ── MC-IMP-005 (manager+config)
                                     │                       │
                                     └───────────┬───────────┘
                                                 ▼
                                        MC-IMP-006 (adapters) ── MC-IMP-007 (trust)
                                                 │                       │
                                                 ▼                       ▼
                                        MC-IMP-008 (commands)    MC-IMP-009 (prompt+CLI)
                                                 │                       │
                                                 └───────────┬───────────┘
                                                             ▼
                                                    MC-IMP-010 (polish+gate)
```

## Sign-off checklist (PR proposal)

- [ ] Mission filter row for MCP is amended in `docs/spec/MISSION.md`
- [ ] `docs/spec/INDEX.md` lists the five MCP specs
- [ ] Spec scenarios mapped to tracker tasks (table below)
- [ ] Red→Green sequence observed per task
- [ ] All gates green; no new lint/analyze findings

## Scenario → task map

| Scenario | Task |
|---|---|
| [MA-01] auto-connect | MC-IMP-005, MC-IMP-010 |
| [MA-02] approval-gated call | MC-IMP-006 |
| [MA-03] listChanged | MC-IMP-004 |
| [MA-04] crash mid-call | MC-IMP-004, MC-IMP-010 |
| [MA-05] user-only prompts | MC-IMP-008 |
| [MA-06] disconnect on exit | MC-IMP-002, MC-IMP-010 |
| [MT-01..03] stdio | MC-IMP-002 |
| [MT-04..06] http | MC-IMP-003 |
| [MP-01..03] adapters/resources | MC-IMP-006 |
| [MP-04..05] prompts | MC-IMP-004, MC-IMP-008 |
| [MU-01..05] commands/get/set | MC-IMP-008 |
| [MU-06] CLI | MC-IMP-009 |
| [MS-01..08] trust | MC-IMP-007 (MS-01/02 also MC-IMP-006) |
