# The loop contract, harness notices, and full-scope context

Session `1792189515823-0` continued (`continue`, 2026-10-18 13:28Z, model
`kilo-auto/free`): 100 tool calls in one turn, 63 upstream 503s, no narration and
no visible close, ended by `empty_after_tools`. The user's question was a
coverage review that was answered twice and never accepted.

Status: proposal — sign-off required before production code (AGENTS.md
Red → Proposal → Sign-off → Green).

Supersedes the loop-guard design in
`hung-suite-loop-and-upstream-retry-2026-10-17.md` (step 4) and amends
`silent-turn-aborts-and-debug-diagnostics-2026-10-17.md` (its claim that a
request failure "still ends the turn with a real error": a *transient* probe
failure now keeps the answer, `c149610`).

## Already landed (not re-planned here)

`2162e40` inactivity hands the decision back · `e08a55a` bounded mock + test
watchdog + `--list`/filter · `c365826` `Stop` value object · `f2782fe`
`process_read wait=N` · `a2dc985` harness warnings · `12f4ea9` terminal restored
on every exit path · `c149610` transient probe outage keeps the answer ·
`b18ea49` the `done` marker matches however it is wrapped · AGENTS.md: coverage
recipe and the `w/` / `.wt/*` worktree warning.

## Problem, measured

- **The harness forced continuation.** `confirm_turn` injects a **fake user
  message** ("Are you finished? … use tools now", `lib/agent.cpp:703-707`) after
  every text-only reply. In the session it appears **20×**, 13× inside the last
  run, always answered by the probe with a tool call (19 of 20). The model
  produced 6 closing markers (`done.` ×4, `**done.**` ×2); **all 6 were
  rejected**; the turn ran to the 100-iteration cap and ended
  `empty_after_tools`.
- **Nothing the model wrote was surfaced as a close.** A text-only reply is
  displayed only through the accepted-probe path (`lib/agent.cpp:882-891`), so
  the two summaries it wrote reached the screen only as streamed text; the
  harness's own stop line reached nobody (the TUI discards `run()`'s return,
  `tui/tui.cpp:412`).
- **113 assistant messages in that run: 100 carried no content at all** (tool
  calls only), 9 were harness error text **in the assistant's voice**
  (`chat_error_reply`, `lib/agent_helpers.cpp:279-285`), and 2 were prose.
- **The model is kept out of the loop's own state**: retry attempts (a
  successful retry is invisible), silent request repairs (tools dropped, model
  swapped, key replaced — `lib/agent.cpp:931-966`), loop/stop reasons
  (`lib/agent.cpp:804`, `:823`, `:1136`), compression failures
  (`lib/compressor.cpp:329-333` returns silently), dropped malformed tool calls
  (`lib/llm.cpp:147-150`), and `on_debug` is unwired in the TUI (only the bench
  recorder sets it).

Industry reference for the fix (see Sources): the loop is "repeat until the
model stops emitting tool calls"; harnesses bound it as a **safety** (AI SDK
defaults to 20 steps and warns when that truncates), never as a demand to
continue; the one harness that refuses a stop caps it (Claude Code, 8
consecutive blocks) because refusal is the trap.

## Decision

Adopt the canonical loop, keep bounding as *reported state*, and give the model
the full scope of what the harness knows.

1. **The model decides when the turn ends.** Continue iff the model requests
   tools; a text-only reply is the answer.
2. **The harness bounds and reports; it never forces.** Bounds are visible
   before they bite and the reason is visible after.
3. **Nothing the harness does is invisible to the model.** Notices are typed and
   in-context: retries, repairs, budgets, stops.
4. **No harness text impersonates the model or the user.**

## Design

**D1 — Turn contract.** Delete the probe: `confirm_turn`, `try_confirm`,
`probe_unavailable_note`, `probe_was_denied`, `is_confirmation_reply`, and the
`chat_with_retry_strict` call site. `advance_turn` ends the turn on any reply
without tool calls and returns that reply. The `done` marker stays a prompt
convention (`prompts/system.md:73-79`), never a gate.

**D2 — Harness notices, typed.** New system-role notice (`[harness] …`) for
everything the harness says: retry summaries, repairs, loop/budget state, stops.
`chat_error_reply` and `internal_failure_reply` change role from `assistant` to
this channel and name the attempt count ("3 attempts over 5s: HTTP 503 …").

**D3 — Empty/progress-only reply after tools: bounded retry.** `empty_after_tools`
becomes up to **2** retries with a system note ("your last reply carried no tool
call and no content; continue with a tool, or state the blocker"), then a stop
whose reason is displayed. (Industry mirror: the Hermes issue in Sources.)

**D4 — Budget and repeats reported.** A one-line system note, injected only when
it matters: ≥80 % of `max_tool_iterations`, ≥80 % of `max_wall_ms` when set, or
the same tool call repeated ≥2× (compared on canonicalised arguments — the
`timeout` argument stripped, per the superseded guard). Content: `step 87/100 ·
wall 12m · last command repeated 3×`.

**D5 — Every stop is visible.** `finish_turn` pushes its reason into the context
as a D2 notice, and the TUI displays the reply `run()` returns instead of
discarding it (`tui/tui.cpp:412`).

**D6 — Plan tool first-class.** `todowrite` already exists (`prompts/tools/plan.md`)
and a tool call keeps the loop alive by D1. Verify: its result is small, it
survives compaction (the prompt already claims this — make it true), and the
prompts say a plan with pending items is a reason to continue, nothing pending a
reason to close.

**D7 — Prompt alignment.** Add one line to `prompts/system.md` naming the
`[harness]` notices so they read as environment facts, and keep the `done`
convention as the natural close.

**D8 — Error transparency.** Per attempt: silent (as today). At exhaustion: one
D2 notice with the count. Disclose what the harness changed on the model's
behalf: tools dropped for a repaired retry, model id switched, API key replaced,
window shrunk by the 400-overflow learner (`lib/http_transport.cpp:136-142`).
Compression classify **and extract** failures (`lib/compressor.cpp:329-333`)
produce one notice each. Malformed streamed tool calls (`lib/llm.cpp:147-150`)
are reported instead of dropped.

**D9 — Diagnostics wired.** The TUI sets `on_debug` (today only `bench/recorder.cpp:76`
does), routing the error journal; the deferred list from
`silent-turn-aborts-and-debug-diagnostics-2026-10-17.md` (MCP/plugin/probe/config-load
failures) is finished.

**D10 — Retry hardening (own PR).** Jitter (±25 %, deterministic from the
request id), growing backoff (1/2/4/8s, capped), honour `Retry-After`
(`http_transport` must expose response headers), and a per-endpoint cool-down
after N consecutive 5xx — reported as state via D4/D2, never a kill. Rationale:
throttling was not proven in the session (no 429, no `Retry-After`, no quota
wording in 59 failures), so this is robustness, not a diagnosis.

**D11 — Process and job reaping (own PR).** Kill path: SIGTERM the group, 2s
grace, SIGKILL (`lib/process.cpp:197` today: SIGKILL only). Drain with a bounded
deadline (never "until EOF"), `waitpid` unconditionally, join the reader before a
job is erased, and terminate + reap every job at host exit.

**D12 — Copy without leaving amber.** A mouse-off toggle (keybinding +
`mousemask(0 …)` next to `tui/tui.cpp:128`, documented in `keybindings.json`),
alongside the restore work already landed.

**D13 — Housekeeping.** Delete dead `safe_chat_once`
(`include/agent/agent_helpers.h:52`, no callers). Document `run_tests --list`,
the positional filter and `AMBER_TEST_TIMEOUT_MS` in `prompts/tools.md`.
Consider a `make coverage` wrapper around the AGENTS.md recipe. Register this
proposal in `docs/fix-tracker.md` and the new issues in `docs/issues.md`.

## Sequence (red → green, one commit each)

1. **D1 + D2 + D7** — turn contract and typed notices. Red: turn ends on a
   text-only reply; no user-role message is injected; a failed generation yields
   a system notice naming the attempts; the marker is not required.
2. **D3 + D5** — empty-after-tools retry, stop reasons in context and on screen.
   Red: an empty post-tool reply retries twice then stops with a displayed
   reason; `run()`'s reply is what the host shows.
3. **D4 + D6** — budget/repeat notes; plan-tool continuation. Red: at 80 % of the
   cap the next request carries the state note; a repeated canonicalised command
   is counted; a `todowrite` call continues the loop.
4. **D8 + D9** — transparency and diagnostics. Red: each silent path in D8 emits
   exactly one notice; a repaired retry is disclosed.
5. **D10** — retry hardening (own PR, baseline-safe for `agent_helpers` size).
6. **D11** — process/job reaping (own PR; tests use a pty child that ignores
   SIGTERM).
7. **D12 + D13** — TUI toggle and housekeeping.

Each step: failing test first, then the change; `make test`, `make check`,
`make complexity|nesting|hygiene|debt|class-size`, `make duplicates` clean.

## Invariants

- **I1** No context message claims a role it does not have: harness text is
  `[harness]` system-role, never `assistant` and never `user`.
- **I2** No harness message asks the model to continue; a text-only reply always
  ends the turn (test-enforced, and grep-enforced: no "use tools now").
- **I3** Every stop has a reason, the reason is in the context, and the host
  displays it.
- **I4** An error notice states how many attempts it took.
- **I5** A bound is announced before it bites (D4) and its firing is reported.
- **I6** A failed internal LLM call (compression, catalog) yields exactly one
  notice; a silent failure is a bug.

## Decisions taken (2026-10-18)

- The probe goes; the model's own close is the close.
- Bounding stays, reported, never forcing; `detection_loop` remains off — D4's
  reporting replaces it.
- Harness notices are a typed channel, not a borrowed voice.
- Empty-after-tools retries are capped at 2.
- Retry hardening (D10) is a separate PR: it is the only change that can mask a
  failure.
- Throttling is not diagnosed; D10 is robustness.

## Evidence

- Probe injections: 20 in the session, 13 in the final run; 19/20 answered with a
  tool call; 6 closing markers rejected; the run ended `empty_after_tools`
  (log `1792330083475.jsonl`, 267 events, 2026-10-18T14:03:47Z).
- Nothing surfaced at the close: `empty_turn_reply`
  (`lib/agent_helpers.cpp:355-365`) reaches `run()`'s caller only, and
  `tui/tui.cpp:412` drops it.
- The 503s: 63 `chat_error` events in the final run, always to attempt 3; the
  upstream body in every case is
  `{"error":"The upstream provider was unable to process the request","error_type":"upstream_error",...}`.
- External: Codex's loop "repeats until the model stops emitting tool calls"
  (OpenAI); AI SDK's four termination conditions and its 20-step safety default;
  OpenCode's expected behaviour "exit cleanly, not call the LLM again" on a
  no-tool-call final message; Claude Code's stop-hook infinite-loop trap capped
  at 8 blocks; the infinite-agentic-loop study naming model-controlled
  continuation without a reachable exit as the risk.

## Sources

- [Unrolling the Codex agent loop — OpenAI](https://openai.com/index/unrolling-the-codex-agent-loop/)
- [AI SDK — Loop Control](https://ai-sdk.dev/docs/agents/loop-control)
- [Claude Code Stop hooks and the infinite-loop trap](https://amitkoth.com/claude-code-stop-hooks/)
- [OpenCode issue: must exit on a no-tool-call final message](https://github.com/anomalyco/opencode/issues/26220)
- [Hermes issue: progress-only post-tool replies must not end the task](https://github.com/NousResearch/hermes-agent/issues/42503)
- [When Agents Do Not Stop: infinite agentic loops (arXiv 2607.01641)](https://arxiv.org/html/2607.01641v1)
