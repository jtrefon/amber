## Spec: MCP Transport (JSON-RPC Wire Contract)

### Purpose

Define the two transport implementations amber uses to talk to MCP servers —
**stdio** (spawned subprocess) and **Streamable HTTP** (remote endpoint) — and
the shared JSON-RPC 2.0 wire layer underneath. This is the boundary where
protocol bytes become typed messages; everything above it (session, adapters,
UI) is transport-agnostic.

Protocol version supported: **2025-06-18** (negotiated at `initialize`).

### Ownership

- **Source files** (target): `include/agent/mcp_transport.h` (port + message
  types), `lib/mcp_jsonrpc.cpp` (framing, encode/decode, correlation),
  `lib/mcp_transport_stdio.cpp`, `lib/mcp_transport_http.cpp`
- **Test files** (target): `tests/mcp_transport_test.cpp`
- **Spec status**: design — implementation tracked in `docs/mcp-tracker.md`
  (MC-IMP-002, MC-IMP-003).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Outgoing `McpRequest`/`McpNotification`; incoming raw bytes (pipe or HTTP body/SSE) |
| **Output** | Incoming `McpMessage` (request, notification, or response); request/response correlation by `id` |
| **Error states** | Spawn failure, parse failure, protocol-version mismatch, HTTP 4xx/5xx, session-id 404 (re-initialize), timeout, transport EOF/disconnect |
| **Invariants** | See below. |
| **Thread safety** | Transport objects are single-threaded; the stdio reader thread is internal and hands messages to a queue drained by the client. |

### Message envelope (shared)

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

- Requests carry `id` (integer). Notifications carry no `id`. Responses carry
  `result` or `error` (`code`, `message`, optional `data`).
- All messages are UTF-8. Unknown fields are ignored (forward compatibility).
- The wire layer maps JSON-RPC errors to `McpError{ code, message, data }`:

| JSON-RPC code | Meaning | Amber handling |
|---|---|---|
| -32700 | Parse error | Transport failure → server `disconnected` |
| -32600 | Invalid request | Transport failure |
| -32601 | Method not found | `ToolResult{ok=false}` with message |
| -32602 | Invalid params | `ToolResult{ok=false}` with message |
| -32603 | Internal error | `ToolResult{ok=false}` with message |
| -32000..-32099 | Server error | Surface as tool error; keep session |
| other | Protocol-specific | Surface; keep session |

---

### stdio transport

**Spawn contract** (new primitive in `lib/process.cpp`, sibling of
`spawn_shell`):

- `spawn_mcp_server(command, args, cwd, stdin_fd, stdout_fd, stderr_fd, err)`
  → fork/exec (no shell) of the server binary with **three** separate pipes:
  child stdin (client writes), child stdout (client reads), child stderr
  (captured, drained, forwarded to the debug log — never to stdout).
- Process group isolation like `spawn_shell`: the whole subtree is killable
  with one `kill_process_group()`.
- The reader thread drains stdout continuously into an internal queue so the
  server can never block on a full pipe.

**Framing:**

- Messages are newline-delimited JSON-RPC on stdout/stdin.
- Incoming bytes are split on `\n`; each complete line must parse as a single
  JSON value; a partial line at EOF is dropped. Embedded newlines in a message
  are a protocol violation → transport failure.
- The client writes each outgoing message as one line (no embedded newlines —
  enforced at serialization: JSON is generated compact).

**Shutdown (MCP lifecycle):**

1. Close stdin (writer side) — signals end of session.
2. Wait up to 3 s for the child to exit.
3. SIGTERM the process group; wait up to 3 s.
4. SIGKILL the process group; reap.

**stderr policy:** captured, size-capped (64 KiB), and forwarded to the amber
debug log when enabled. Never rendered into the conversation.

#### [MT-01] stdio framing round-trip

- **Given**: a fake server binary that echoes JSON-RPC lines (test fixture)
- **Input**: client sends `initialize` request
- **Expected**: server receives exactly one newline-delimited message; the
  client parses the echoed response; ids match.
- **On failure**: Framing corruption (embedded newlines) → transport error, not
  a hang.

#### [MT-02] stdio shutdown escalates to SIGKILL

- **Given**: a server that ignores SIGTERM (test fixture)
- **Input**: client disconnects
- **Expected**: stdin closes, SIGTERM sent, SIGKILL sent after the deadline,
  process reaped, no zombie.
- **On failure**: Orphaned server process.

#### [MT-03] stdio stderr never leaks into the protocol

- **Given**: server logs "hello world" to stderr and answers normally on stdout
- **Input**: any request
- **Expected**: Response parsed cleanly; stderr text appears only in the debug
  log.
- **On failure**: stderr bytes corrupt the response stream.

---

### Streamable HTTP transport

**Endpoint contract:**

- One URL (`https://host/mcp`); every client message is an HTTP POST; the
  server replies `application/json` (one JSON-RPC message) or
  `text/event-stream` (SSE).
- Required headers on every request:
  - `Accept: application/json, text/event-stream`
  - `MCP-Protocol-Version: <negotiated>` (after initialize)
  - `Mcp-Session-Id: <id>` (after the server assigns one)
  - `Authorization: Bearer <token>` when the server config has `auth_token`
- `notifications/initialized` and client notifications: POST → expect `202`
  (or a JSON-RPC error body without `id`).

**SSE handling** (reuses `lib/sse_parser.cpp`):

- A POST that returns `text/event-stream` is drained until the JSON-RPC
  response for that request arrives (or the request timeout fires). Server
  requests/notifications sent before the response are queued to the client.
- A long-lived GET stream (server-initiated messages) is **not opened in v1**:
  server requests/notifications are delivered from POST response streams
  (the spec permits this). `listChanged` is picked up on the next client
  operation, so discovery stays fresh without a persistent connection.
- v1 **does not** implement stream resumption (`Last-Event-ID`) or
  `Mcp-Session-Id`-based 404 re-initialization races beyond a single retry:
  on 404 the client sends a fresh `initialize` and re-runs discovery.

**Session id:**

- Captured from the `Mcp-Session-Id` header of the `initialize` response (if
  present) and echoed on all subsequent requests.
- Termination: `DELETE` with the session id (server MAY 405; ignored).

#### [MT-04] HTTP JSON round-trip

- **Given**: a test endpoint (Python fixture or a canned local server) that
  answers `initialize` with `application/json`
- **Input**: `initialize` request
- **Expected**: Response parsed; headers echoed per contract.
- **On failure**: Non-JSON body or wrong status → typed transport error.

#### [MT-05] HTTP SSE streaming response

- **Given**: endpoint answers a `tools/call` with `text/event-stream`; the
  response arrives in the third SSE event
- **Input**: the call
- **Expected**: All SSE events before the response are queued (notifications
  routed, requests answered with `-32601` or deferred); the response is
  returned once parsed.
- **On failure**: Response missed because earlier events were dropped.

#### [MT-06] Session expiry re-initializes

- **Given**: server returns HTTP 404 for a request carrying a stale
  `Mcp-Session-Id`
- **Input**: the request
- **Expected**: Client sends a fresh `initialize`, re-runs discovery, and retries
  the request once. Second failure → tool error.
- **On failure**: Stale session silently fails every call.

---

### Timeouts and cancellation

- **Request timeout**: default 60 s per request, configurable per server
  (`timeout_s`). Initialization: 10 s fixed.
- **Progress**: `notifications/progress` resets the idle clock; the hard
  timeout always applies (spec RECOMMENDED behaviour).
- **Cancellation**: amber's `CancellationToken` maps to
  `notifications/cancelled` (`requestId`, `reason`). In-flight tool calls poll
  the token; on cancel, the client sends the notification and stops waiting.
  The server may still deliver a late response, which is discarded.

---

### Cross-references

- **Depends on**: `mcp/mcp-architecture.md`, `docs/spec/llm-client/streaming.md` (SSE reuse via `lib/sse_parser.cpp`), `lib/process.cpp` (spawn primitives)
- **Depended on by**: `mcp/mcp-client.md`, `mcp/mcp-security.md`
- **Test coverage**: `tests/mcp_transport_test.cpp` (MT-01..06), plus fixture servers under `tests/fixtures/`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (2025-06-18 wire contract; stdio + Streamable HTTP; no legacy HTTP+SSE transport, no resumption) |
