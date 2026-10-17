# Silent turn aborts and debug-gated diagnostics

Issue: a bash call that hits its timeout kills the entire turn with a JSON type
error, and nothing — no transcript event, no log line — records why. This
proposal closes the unsafe-read class behind it, makes a turn unkillable by a
non-chat exception, and puts every non-session diagnostic behind a single
`debug` switch. (New issues.md entry to be registered with this proposal.)

Status: proposal — sign-off required before production code (AGENTS.md
Red → Proposal → Sign-off → Green).

## Implementation status (2026-10-17)

Landed on `fix/json-type-guarded-reads`, verified with `make test`
(1124 passed; the 3 failures are this branch's own red compression-sizing
tests), `make complexity|nesting|hygiene|debt|class-size`, `make duplicates`,
`make format-check-changed` (clang-format 18.1.3, the runner's pin).

| PR | State |
|---|---|
| 1 — the abort | **done**: `bash_tool` records a boolean flag (the *writer* was wrong, not the reader: the envelope and `bench/recorder.cpp:98` both read the key as a flag; the seconds stay in `error`/`output`). The per-iteration guard (`fold_reply` → `recover_internal_failure`) reports and survives a non-chat failure, answers the tool_calls it swallowed, and stops after 2 repeats. `RequestFailedError` (new, `llm.h`) keeps the strict probe's escalation propagating — a request failure still ends the turn with a real error. |
| 2 — the class | **done**: `read_int`/`read_object`/`read_array` join `read_bool`/`read_string`; every site in problem 2 converted (tool numeric args, tool string args, MCP replies and schema, plugin replies, the persisted stores, bench recorder). `compressor_parser.cpp`'s private `read_string` deleted in favour of the shared one. |
| 3 — the switch and the log | **partial**: `Config::debug` (default off, `AMBER_DEBUG_ON`) + `/set debug on|off` (registry row, both actions, `completions.json`); the error journal (`logs/errors.jsonl`, next to the transcripts, one row in `config_paths()` so the readout and the writer cannot drift) records `chat_error`, `error`, and the host's `uncaught` failures, rotating at 1 MB keeping one previous; the raw HTTP/SSE dump now follows the same switch (`Config::debug_dump_path()`). **Deferred:** routing MCP/plugin/probe/config-load failures into the journal — they neither throw nor log today, so each needs the journal handed to it. |
| 4 — transcript identity | **not started**: the session id is still assigned at the first autosave while the transcript is named at the first prompt, so one conversation can still span two files. |
| — extra finding | **fixed**: a plugin that exits between requests killed the host with SIGPIPE (`lib/plugin.cpp` spawn path now ignores it, mirroring `mcp_transport_stdio.cpp:51`) — surfaced by the plugin test. |


## Problem

### 1. A tool timeout kills the turn, deterministically

`BashTool` writes the timeout **in seconds, as an int**, into the result meta
(`tools/bash_tool.cpp:138`), and the tool-result envelope reads that key **as a
bool** (`lib/agent_helpers.cpp:61`, before the working-tree fix):

| Step | Site | Value |
|---|---|---|
| Writer | `tools/bash_tool.cpp:138` | `r.meta["timeout"] = timeout;` → `60` (number) |
| Reader | `lib/agent_helpers.cpp:61` | `meta.value("timeout", false)` → expects boolean |
| Throws | nlohmann | `[json.exception.type_error.302] type must be boolean, but is number` |

The read sits in `format_tool_envelope`, called from `finish_call`
(`lib/dispatch.cpp:463`) **after** the `tool_result` event has been logged
(`:454`) and **outside** any try/catch — so the exception escapes
`dispatch_tool_calls` and `Agent::run()` entirely. The worker catch
(`tui/tui.cpp:375-383`) turns it into an `AgentEvent::Error`, which
`EventRouter::on_error` (`tui/event_router.cpp:465-471`) renders as a transient
`error: …` line in the window and nothing more.

Consequences, all observed:

- The turn ends where the tool ended: no model reply, no `turn_end`, no
  `error` event, and a trailing **empty assistant message** in the session file.
  A user asking a question gets silence.
- The `error: empty_after_tools` marker (`lib/agent.cpp:922-935`) never runs,
  because the abort never reaches `finish_turn`.
- Every timed-out bash call does this. Measured across the local transcripts:
  **5 bash timeouts, 5 dead turns** — 4 are literally the last event of their
  transcript, the 5th is followed only by a new `user` prompt.

Reproduction against the vendored header:

```
meta["timeout"] = 60;  meta.value("timeout", false)
  → THROWS: [json.exception.type_error.302] type must be boolean, but is number
```

Latent since 2026-07-27 (`2d21bd3` introduced the envelope read; `977e13a`
introduced the int write). It is now frequent because the agent runs long
commands (`make test`, `run_tests`) whose silence exceeds the 60 s idle budget —
i.e. the crash fires exactly when a turn is doing real work.

### 2. The unsafe-read class is wider than the one site

The same shape — a foreign payload read with a raw typed default — exists at
these remaining sites. Reachability measured, not assumed:

| Site | Read | `"60"` | `60.0` | `true` | `60` |
|---|---|---|---|---|---|
| `tools/bash_tool.cpp:91` | `a.value("timeout", 60)` | 💥 | ok | ok | ok |
| `tools/process_tool.cpp:96-97` | `timeout`, `idle_timeout` | 💥 | ok | ok | ok |
| `tools/read_tool.cpp:38-39` | `offset`, `limit` | 💥 | ok | ok | ok |
| `tools/search_tool.cpp:145` | `max` | 💥 | ok | ok | ok |
| `lib/mcp_client.cpp:71-73, 208` | `listChanged`, `isError` (bool) | — | 💥 (`1`/`0`) | ok | — |

💥 = `type_error.302`. These throw *inside* the tool, so dispatch converts them
to a logged `tool threw: …` — the call is lost but the turn survives. The
envelope read above is the only one that is turn-fatal, because it runs after
the tool has returned.

Already converted in the working tree (uncommitted, branch
`fix/json-type-guarded-reads`): `agent_helpers` `denied`/`timeout`,
`mcp_client` `required`, `memory_persistence` `promoted` ×2, `plugin`
`enabled`/`ok` ×2, `process_tool` `all` — via the adopting readers in
`include/agent/json_read.h`.

### 3. Failures vanish, and the logs have no gate

What exists today:

- **Session transcript** — default-on, one JSONL per session at
  `Workspace::state_dir() + "/logs/{ts}.jsonl"` (`lib/agent.cpp:682-691`,
  `lib/conversation_log.cpp:15-30`; `{ts}` is the open time). Carries
  `session_start`, `user`, `assistant`, `tool_call`, `tool_result`,
  `tool_denied`, `chat_error`, `tool_recovery`, `error`, `turn_end`.
- **Debug dump** — opt-in by path (`Config::debug_log`,
  `include/agent/config.h:103-107`, `AMBER_DEBUG` at `lib/config.cpp:428`);
  raw HTTP bodies/SSE, truncate-on-first-write (`lib/debug_log.cpp`).

What does not exist:

- No error log. The state root contains only `projects/`; grepping every log and
  session for `type must be`, `bool was expected`, or an error-text `tool threw`
  finds **nothing** — the type errors never reached disk.
- The abort path (problem 1), the TUI `on_error` surface, and everything that
  happens **before the first prompt opens the transcript** (probe, model list,
  catalog, MCP/plugin handshake) have no sink at all.
- The transcript is not continuous: the coverage session's two turns live in two
  different files (`1792189316598.jsonl`, `1792190682622.jsonl`) and its session
  id changed mid-conversation (`1792189515823-0`, assigned at 00:25:15 by the
  first autosave, `tui/tui_session.cpp:113-127`) — the log name is wall-clock at
  open, the id is wall-clock at first save. Cost: archaeology by hand.

Directive for this proposal: **no always-on logging beyond the session
transcript.** Amber's directive is performance and latency; diagnostics are
opt-in, under `/set debug on`. "This should be for all our logs except session."

## Decision

1. **Guard the loop, not the call sites.** One wrapped iteration in `run()`
   makes a non-chat exception recoverable and recorded, in the same way
   `safe_chat_once` (`lib/agent_helpers.cpp:173-192`) already handles chat
   errors. A single throwing read can then cost one turn's *value*, never the
   turn's *existence* — and the class grows by omission instead of by crash.
2. **One adopting reader family, applied to the class.** Extend
   `json_read.h` with `read_int`/`read_number` (adopt `"60"`, `60.0`, `true`)
   and use `read_bool`/`read_string` everywhere a foreign field is read —
   including the MCP bools. Adoption, not guard-and-default: a numeric `1`/`0`
   where a bool is declared *is* that boolean.
3. **One `debug` switch** (default off) gates every diagnostic sink; the session
   transcript stays as it is. When off, a diagnostic path costs one bool test —
   no formatting, no JSON, no file open.

## Design

### 1. The per-iteration guard

In `Agent::run()`, wrap one loop iteration (post-chat bookkeeping, dispatch,
envelope) in a try/catch for `std::exception`, mirroring `safe_chat_once`:

- log `error` with `reason`, `stage`, and `what()` to the transcript (so the
  record always ends with a terminal event);
- push an assistant message the model can see and recover from
  (`[error while <stage>: <what()>] …`);
- continue the loop; the turn ends normally through `finish_turn`, so
  `turn_end` is always the last event of a transcript.

Cancellation keeps its own path (`finish_turn_cancelled`, `lib/agent.cpp:912`).

### 2. Reader family and site conversion

`include/agent/json_read.h` grows a numeric sibling to `read_bool`:

```cpp
inline long long read_int(const json& obj, const char* key, long long fallback);
```

Adopting semantics: number → value; string → parsed when numeric, else
fallback; bool → 1/0; missing/uninterpretable → fallback. Every site in the
table in problem 2 converts. `meta["timeout"]` is the one case where the
**writer** is fixed instead: it is a flag by contract (the envelope and
`bench/recorder.cpp:98` read it as one), so `bash_tool` writes `true` and the
seconds stay in `error`/`output`; `read_bool` at the envelope remains as
defence for a legacy or third-party writer, and adopts a number as the timeout
it means.

### 3. The `debug` setting (plumbing, end to end)

Same shape as `detection.loop`, which is the established template:

| Layer | Site | Change |
|---|---|---|
| Config field | `include/agent/config.h:114-119` | `bool debug = false;` (default off) |
| Parse / save / env | `lib/config.cpp:144-145, 382, 427-428` | `debug` key, `debug=1` on save, `AMBER_DEBUG` |
| Settings row | `tui/tui_input.cpp:2794-2815` (`build_settings`) | `debug`, `Choice`, `<on|off|toggle>` |
| Actions | `tui/tui_input.cpp:926, 1104` | `register_action` for `core.config.set.debug` / `.get.debug` |
| Command tree | `completions.json:196, 394` | `get.debug` / `set.debug` nodes with help + choices |

No hardcoded command path, no hardcoded choice list — the tree and the registry
stay the only surface (AGENTS.md).

### 4. Gating table

| Sink | Default | Under `debug on` |
|---|---|---|
| Session transcript (`logs/<ts>.jsonl`) | **on** (unchanged) | on |
| Error log (`logs/errors.jsonl`) | off — no file, no buffer | on |
| Debug dump (`debug_log`) | off (path empty) | path derived: `state_dir()/logs/debug-{ts}.log` |
| Future diagnostic sinks | off | on |

Perf contract: the gate is an inline bool test at sink entry, before any
formatting; nothing is built, opened or rotated while off. The session
transcript is the only always-on writer, and it already writes one line per
event, not per token.

### 5. Error log shape

Append-only JSONL at `<state>/logs/errors.jsonl`, opened on the first record
(so it never exists when nothing went wrong), one object per failure:

```json
{"ts":1792190923338,"kind":"turn_abort","stage":"dispatch","error":"…",
 "session":"1792189515823-0","model":"kilo-auto/free",
 "last_tool":{"name":"bash","args":{"command":"w/run_tests"},"ok":false},
 "source":"lib/dispatch.cpp:463","pid":123456}
```

Lifecycle (per the standing requirement that what amber creates on disk has a
lifecycle): rotate at 1 MB, keep one previous, record `pid`, never reaped by a
second instance. Path registered in `config_paths()` and the amber-config skill
body so it cannot drift from the layout.

### 6. Chokepoints routed into it

| Failure | Today | After |
|---|---|---|
| Turn abort (problem 1) | window line only | error log + `error` transcript event + recovering assistant message |
| `safe_chat_once` chat error | transcript `chat_error` | + error log |
| `tool threw` (`lib/dispatch.cpp:417-418`) | transcript `tool_result` | + error log |
| `publish_error` / `ApiError` (`lib/agent.cpp:905`) | window line | + error log |
| Probe / catalog / MCP / plugin handshake | nothing | error log (§5 is open before the transcript) |
| Config / memory / policy load | cryptic or nothing | error log with the offending path |

## Sequence (each PR red → green)

1. **PR1 — the abort.** Red: a test that a timed-out bash result with int meta
   does not abort the turn and that the transcript ends with `turn_end`. Green:
   the reader fix + the per-iteration guard. Smallest change that stops the
   bleeding; unblocks everything else.
2. **PR2 — the class.** `read_int`/`read_number` + the site table from problem 2,
   one test per site's reachability row (string/float/bool/number).
3. **PR3 — the switch and the log.** `debug` setting end to end, error log with
   rotation, chokepoints from §6, tests for write/read/rotate and for "off =
   no file created".
4. **PR4 — transcript identity.** Assign the session id at window creation so
   the log name and the session id cannot drift, so one conversation is one
   transcript file.

## Invariants

- A turn always ends with a terminal transcript event, and never with an empty
  assistant message and no explanation.
- `debug off` creates no file, allocates nothing, and opens nothing on any
  diagnostic path; the session transcript is unchanged.
- No log sink grows without bound.
- A foreign `1`/`0`/`"true"`/`"60"` never costs a call, let alone a turn.
- Every new setting appears in `completions.json` and the registry — no
  hardcoded command path, no hardcoded choice list.

## Open decisions (need sign-off)

1. **Post-mortem when `debug` is off.** With the switch off, a fatal abort
   leaves nothing behind — which is the case we most want to see. Option: a
   fixed 64-entry in-memory ring of error records (no I/O; one bounded copy per
   *error*, never per event), flushed to the error log when `debug` is on or on
   a fatal abort. Recommended: ring, because the cost is per failure and it
   preserves the "performance first" default. Decline → nothing is kept when
   off, and diagnosis requires reproducing with `debug on`.
2. **Rotation numbers** (§5): 1 MB / keep 1 as proposed, or a different budget.
3. **Scope of PR4** — fold the transcript identity fix in, or register it
   separately and ship PR1-PR3 first.

## Evidence

- Session `1792189515823-0` ("please review test coverage of our project"),
  turns 1 and 2: `logs/1792189316598.jsonl` (00:21:56 → 00:25:15) and
  `logs/1792190682622.jsonl` (00:44:42 → 00:48:43); both end at
  `tool_result ok=false "timed out after 60s"`; 29 bash calls, 1 of 30 assistant
  messages non-empty; last message is an empty assistant.
- Correlation over all local transcripts: 5 bash timeouts, 5 dead turns.
- Repro (`nlohmann/json.hpp` from `include/`):
  `THROWS: [json.exception.type_error.302] type must be boolean, but is number`.
- Reachability matrix in problem 2 (compiled, not inferred).
- Absence: no `type must be`, `bool was expected` or error-text `tool threw`
  anywhere under `$XDG_STATE_HOME/amber/projects/`.
