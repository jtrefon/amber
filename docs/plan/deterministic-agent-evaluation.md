# Deterministic agent evaluation — simulation, trace replay and E2E

**Status:** proposed direction, tracked by epic [#628](https://github.com/jtrefon/amber/issues/628).
**Milestone:** `deterministic-agent-eval`.
**Scope owner:** benchmark / quality.
**Decision:** no live model in CI. The model is an input fixture, not a
dependency.

This document is the shared design record for the epic. It is written so an
independent session can pick up one child issue without conversation history.

---

## 1. Why this exists

The current benchmark is strong at proving that **code** works. It is not yet a
test of whether the **agent** works.

Measured on `main` at the time of writing:

| Layer | Current state | What it proves | What it misses |
|---|---|---|---|
| Unit + probes | 42 harness probes, large `run_tests` suite | parsers, loop, dispatch, budgets, recovery primitives | no real binary, no multi-step task |
| Hermetic bench | 61 scenarios / 19 suites; 43 hermetic (40 scripted + 3 multi-turn), 18 live-only; 46 matrix cells | the harness executes a **known** plan and scores it | the plan never reacts to a tool result |
| CLI E2E | `bench-e2e`, 5 scenarios through `amber-cli` + mock HTTP | boot, config, plugins, tools, tool sequence | no final workspace, no answer, no exit-code assertion |
| TUI E2E | `tui_pty_test`, 75 tests, real pty + fake endpoint | UI state, modals, providers, approvals, some agent turns | fake scripts use `bash`/`process_start`/`watch`/`delegate`/`tdd`; no read/write/verify journey |
| Live | manual release artifact | model quality | correctly outside CI; unreliable and costly as a gate |

Two concrete gaps follow:

1. **Fixed scripts do not test the feedback loop.** `fake_replies` is a FIFO
   transcript. If the harness drops a tool result, mislabels a result, or fails
   to feed an error back, the script still runs. The benchmark cannot tell a
   real recovery from a rehearsed one.
2. **No end-to-end task completion.** No test starts from a goal, performs a
   multi-step change through the real binary, and asserts the final workspace.
   `tests/e2e_test.cpp` is a `CommandLine` + `MockView` test despite its name;
   it never starts a process.

The TUI framework is **not** the gap. `tests/tui_pty_test.cpp` already has the
hard parts: a real pty, the real binary, `TERM`/`ESCDELAY` handling, exit-ready
detection, force-kill accounting and coverage-safe teardown. It needs to be
extracted and reused, not replaced.

---

## 2. Decision

Build a deterministic **agent simulator** layer around the existing harness:

```
scenario JSON
   │
   ├── in-process bench ──► PolicyClient / FIFO FakeClient ──► Agent ──► tools
   │                                                        └──► oracle + workspace
   │
   ├── CLI E2E ──► amber-cli ──► ScriptedEndpoint ──► Policy/Trace responder
   │
   └── TUI E2E ──► amber (pty) ──► ScriptedEndpoint ──► Policy/Trace responder
```

One scenario definition, three execution surfaces. The same policy can run
in-process or over HTTP; the same trace can be replayed through the real
client.

### Principles

- **Deterministic by construction.** No wall-clock assertions beyond generous
  ceilings, no random seeds without a recorded seed, no network beyond
  loopback, no live model.
- **The model is a fixture.** A policy or a trace is checked in and versioned.
  Model churn cannot break the gate.
- **Closed loop over transcripts.** The fake model reads the conversation and
  the last tool result before deciding the next step.
- **Assert outcomes, not prose.** Final files, exit codes, tool status, context
  invariants and request shape are the oracle. Text checks are a supplement.
- **Test the tests.** The policy zoo and fault matrix must prove the scorer and
  harness can fail, not only pass.
- **One harness.** Extend `bench/` and the existing pty driver. No second
  benchmark, no engine changes.

---

## 3. Architecture

### 3.1 Shared `ScriptedEndpoint` — [#630](https://github.com/jtrefon/amber/issues/630)

`bench::MockServer` is a FIFO, OpenAI-only loopback server. The pty test has a
second hardcoded endpoint. Both become one shared implementation.

`bench/scripted_endpoint.{h,cpp}`:

```cpp
struct EndpointRequest {
    std::string method;
    std::string path;
    std::string body;
    agent::json json;
    std::vector<agent::Message> messages;
    agent::json tools;
    bool stream = false;
    int sequence = 0;
    std::map<std::string, std::string> headers;
};

struct EndpointReply {
    int status = 200;
    std::string content;
    agent::json tool_calls;
    std::string reasoning;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    std::string finish_reason;
    int delay_ms = 0;
    int drop_after_chunks = 0;
    bool malformed_json = false;
    bool disconnect = false;
};

using Responder = std::function<EndpointReply(const EndpointRequest&)>;
```

Required behavior:

- FIFO, callback, policy and trace responders.
- OpenAI `/v1/models` and `/v1/chat/completions`; Anthropic `/v1/messages`;
  Gemini `/v1beta/models/...`; raw passthrough for provider-specific bodies.
- JSON and SSE; split chunks; usage; reasoning; tool-call deltas.
- Deterministic faults: HTTP status/retry-after, delay, disconnect, malformed
  JSON, empty body, drop after N chunks.
- Request log and request assertions, available to the test after the run.
- Ephemeral loopback port; clean shutdown; no external network.

`bench::MockServer` becomes a compatibility wrapper. The pty test uses the
shared server.

### 3.2 Closed-loop `PolicyClient` — [#631](https://github.com/jtrefon/amber/issues/631)

`bench::FakeClient` pops a script. A policy computes the next reply from the
conversation.

```cpp
struct PolicyContext {
    int step = 0;
    const std::vector<agent::Message>& messages;
    const std::vector<std::shared_ptr<agent::Tool>>& tools;
    const agent::json& last_tool_result;
    const Scenario& scenario;
};

struct PolicyReply {
    std::string content;
    agent::json tool_calls;
    std::string reasoning;
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

class Policy {
public:
    virtual ~Policy() = default;
    virtual PolicyReply next(const PolicyContext&) = 0;
};
```

`PolicyClient : agent::LLMClient` supports streaming and non-streaming and
produces the same message sequence in both modes.

The JSON state machine:

```jsonc
"policy": {
  "initial": "read_task",
  "max_transitions": 16,
  "states": {
    "read_task": {
      "call": {"tool": "read", "args": {"path": "TASK.md"}},
      "next": "edit"
    },
    "edit": {
      "call": {"tool": "write", "args": {"path": "answer.txt", "edits": []}},
      "when": {"last_tool_status": "ok"},
      "next": "verify"
    },
    "verify": {
      "call": {"tool": "bash", "args": {"command": "make test"}},
      "next": "done"
    },
    "done": {"final": "done"}
  },
  "on_error": {"retry": 1, "next": "read_task"}
}
```

Conditions: `last_tool_status` (`ok`/`error`/`denied`), `last_tool_name`,
`last_result_contains`, `step`, `always`. Actions: `call`, `final`, `fail`,
`retry`. Validation rejects unknown states, unreachable terminals and unbounded
cycles.

### 3.3 Policy zoo — [#632](https://github.com/jtrefon/amber/issues/632)

One good policy is not enough; the benchmark must discriminate.

| Policy | Behavior | Expected |
|---|---|---|
| `planner` | state-machine plan | passes the reference scenario |
| `lazy` | answers without reading | fails checks |
| `wrong_tool` | bash instead of read/search | lower tool-choice/efficiency |
| `hallucinator` | invents content | fails factual checks |
| `quitter` | stops after first error | fails the task |
| `over_eager` | writes before reading | penalized for the missing step |
| `retrier` | retries one failed call | passes with a recovery record |

`--policy-matrix` runs every policy over a scenario. A committed
`bench/policies/expectations.json` records pass/fail, score bands and required
failure reasons. A scorer regression turns at least one matrix assertion red.
No policy may saturate.

### 3.4 Scenario assertions — [#633](https://github.com/jtrefon/amber/issues/633)

The runner gains three field groups:

```jsonc
"expected_workspace": {
  "files": {
    "answer.txt": {"exact": "done\n"},
    "src/main.cpp": {"contains": "return 0;"}
  },
  "absent": ["scratch.tmp"],
  "diff_contains": ["-old", "+new"]
},
"request_contract": {
  "system_prefix_stable": true,
  "tools_include": ["read", "write"],
  "tools_exclude": ["bash"],
  "no_system_after_first": true,
  "cache_breakpoints": 2
},
"faults": [
  {"at_request": 1, "kind": "http_429", "retry_after_s": 1},
  {"at_request": 3, "kind": "stream_drop", "after_chunks": 2}
]
```

`expected_workspace` supports exact, contains, regex, absent and diff
assertions. `request_contract` catches a volatile system block, a dropped tool
schema and a broken cache breakpoint before a downstream oracle can hide them.
`faults` are request-indexed and one-shot.

### 3.5 Trace replay — [#637](https://github.com/jtrefon/amber/issues/637), [#638](https://github.com/jtrefon/amber/issues/638)

A trace is a pinned provider exchange:

```jsonc
{
  "schema": 1,
  "provider": "anthropic",
  "dialect": "anthropic",
  "source": "manual capture 2026-11-16; key redacted",
  "exchanges": [
    {
      "match": {"sequence": 0, "path": "/v1/messages", "last_user_contains": "fix"},
      "request": {"method": "POST", "path": "/v1/messages", "body": "{...}"},
      "response": {"status": 200, "content_type": "text/event-stream", "sse": ["..."]}
    }
  ]
}
```

Two modes:

- **strict:** the request fingerprint (system prefix, tools, roles) must match;
  report the first drifted field.
- **shape:** path/sequence/predicate match; assert roles and tool names, not
  prose.

The corpus covers OpenAI, Anthropic and Gemini; streaming and non-streaming;
tool calls; cache counters; 429; malformed JSON; empty replies; truncated SSE.
Capture is manual (`amber-bench record --live --trace-out`), redacts secrets and
never runs in CI. The gate replays only.

### 3.6 CLI E2E — [#634](https://github.com/jtrefon/amber/issues/634), [#645](https://github.com/jtrefon/amber/issues/645)

`bench/binary_driver.cpp` gains final-answer, workspace, exit-code, usage and
request-contract assertions. It already runs the real `amber-cli` against a
loopback server and compares tool sequences; it now compares outcomes.

Initial journeys:

1. read → edit → compile/test → done;
2. search → two-file edit → verify;
3. wrong path → error → recover;
4. approval denied → read/write fallback;
5. two-turn follow-up with context carry;
6. approval allowed → bash marker;
7. cancel during a slow tool → clean exit;
8. session resume;
9. skill activation;
10. MCP tool call;
11. plugin-provided tool;
12. subagent delegation;
13. compression mid-task.

#634 lands journeys 1–5; #645 adds 6–8, 12 and 13; #639 adds 9–11.
`make agent-e2e` runs the combined suite. A wrong file, extra file, missing
file, wrong tool, wrong exit code or missing telemetry fails the journey.

### 3.7 TUI E2E — [#635](https://github.com/jtrefon/amber/issues/635), [#636](https://github.com/jtrefon/amber/issues/636)

Extract the `Tui` pty driver and fixture from `tests/tui_pty_test.cpp` into
`tests/pty_harness.{h,cpp}`. Replace `FakeModelsEndpoint` with the shared
`ScriptedEndpoint`. Add `tests/tui_agent_e2e_test.cpp`.

Journeys assert visible text, workspace state, tool calls and exit code:

1. prompt → read → edit → verify → done;
2. approval denied → fallback;
3. approval allowed → marker file;
4. cancel during a tool → no partial artifact;
5. session tab switch while an agent runs;
6. restart → resume → second turn;
7. subagent → owner report;
8. skill activation;
9. MCP/plugin tool;
10. compression → UI stays responsive.

No pixel-perfect screen goldens. Assert semantic text and state; the ncurses
layer already has its own unit tests.

---

## 4. Test strategy — maximum regression coverage

### 4.1 Layers

| Layer | What it runs | Speed | Where |
|---|---|---|---|
| Unit | endpoint, policy, trace, schema, matchers | ms | `tests/bench_test.cpp` |
| Integration (in-process) | runner + policy + real tools | seconds | `tests/bench_test.cpp` |
| CLI E2E | real `amber-cli` + endpoint + real tools | seconds/journey | `make agent-e2e` |
| TUI E2E | real `amber` in a pty + endpoint | seconds/journey | `make tui-agent-e2e` |
| Trace replay | real HTTP client + pinned wire samples | ms/exchange | `make trace-check` |
| Fault/property | faults, seeded sequences, invariants | seconds | `make agent-check` |
| Regression | baselines, score bands, exact traces | ms | CI gate |

### 4.2 Coverage matrix

| Area | Unit | Integration | CLI E2E | TUI E2E | Negative/fault | Regression |
|---|---|---|---|---|---|---|
| Shared endpoint | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Policy client | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Policy zoo | ✅ | ✅ | — | — | ✅ | score bands |
| Scenario schema | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Workspace assertions | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Request contract | ✅ | ✅ | ✅ | ✅ | ✅ | prefix hash |
| Trace replay | ✅ | ✅ | ✅ | ✅ | ✅ | exact |
| CLI journeys | — | ✅ | ✅ | — | ✅ | tokens/calls/wall |
| TUI journeys | — | ✅ | — | ✅ | ✅ | text/state |
| Skills / MCP / plugins | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Long-horizon repo | — | ✅ | ✅ | — | ✅ | baseline |

### 4.3 Negative and fault matrix

At minimum:

- endpoint: missing reply, unknown path, duplicate sequence, malformed JSON,
  empty body, 429/500, disconnect before headers, disconnect mid-SSE, truncated
  SSE, delay beyond deadline, tool-schema 400;
- policy: unknown state, unreachable terminal, unbounded cycle, malformed
  condition, step cap, empty reply, retry exhausted;
- workspace: wrong content, missing file, extra file, invalid regex, diff
  mismatch;
- request contract: system prompt drift, missing tool, extra tool, system
  message after the first, wrong cache breakpoint;
- trace: missing exchange, extra request, wrong predicate, strict fingerprint
  drift, secret-like value, malformed provider body;
- CLI: wrong exit code, missing telemetry, timeout, denied approval that hangs,
  cancel after write;
- TUI: wrong tab, approval dialog never answered, cancel with a draft, session
  resume missing, subagent report to the wrong owner;
- surfaces: unknown skill, MCP timeout, plugin crash, malformed manifest.

### 4.4 Property and fuzz

- Seeded policy sequences: at least 1,000 generated valid tool-call sequences
  with a fixed seed; assert no crash, hang, duplicate result or context
  corruption.
- Invariants: every dispatched call has exactly one result; context hash chain
  is intact; budgets hold; workspace confinement holds.
- Fuzz the trace loader and SSE parser with the existing libFuzzer setup where
  practical. The fuzz target fails closed when the compiler lacks libFuzzer.

### 4.5 Regression baselines

- `bench/results/agent-e2e-baseline.json`: per journey tokens, calls, wall,
  success. Fail above +10% tokens/calls, a success drop, or a 2x + 500 ms wall
  ceiling. Missing baseline scenario fails closed.
- `bench/results/policy-matrix-baseline.json`: score bands and pass/fail per
  policy. A policy outside its band fails.
- Trace replay is exact; no baseline tolerance.
- TUI assertions are semantic text and workspace state; no pixel diffs.
- Coverage: patch ≥80% lines, branch ratchet for new decision lines.

### 4.6 Flake rules

- Wait on conditions (`agent::test::wait_until`), not fixed sleeps.
- Wall-clock assertions need a named regression and a generous ceiling;
  `make flake-lint` applies.
- TUI sessions must exit through `AMBER_EXIT_READY`; a SIGKILL is a failure.
- New suites run under `--repeat 2` before the gate is enabled.
- A quarantined test is explicit and tracked, not silently skipped.

---

## 5. CI and gates

New gating jobs:

| Job | Local command | Dimension | Timeout |
|---|---|---|---|
| `agent-e2e` | `make agent-e2e` | cpp | 30 min |
| `tui-agent-e2e` | `make tui-agent-e2e` | cpp | 30 min |

Both jobs:

- build `amber-cli`, `amber-bench` and the TUI;
- run the deterministic suites with no live model;
- compare baselines and fail closed on a missing report;
- upload the report and append the table to `$GITHUB_STEP_SUMMARY`;
- be listed in `tools/gates_manifest.py`, `ci-gate.needs`,
  `tools/gate_needs_check.py` and `tests/build_hygiene.sh` P15;
- have a local command (parity invariant);
- use `ubuntu-24.04`, not `ubuntu-latest`.

The existing `bench-efficiency` and `bench-e2e` jobs remain. The new jobs add
behavioral coverage; they do not replace the hermetic overhead gate.

---

## 6. Phases and issue map

| Phase | Issue | Outcome | Depends on |
|---|---|---|---|
| 0 | [#629](https://github.com/jtrefon/amber/issues/629) | plan and design record | — |
| 0 | [#630](https://github.com/jtrefon/amber/issues/630) | shared `ScriptedEndpoint` | — |
| 0 | [#631](https://github.com/jtrefon/amber/issues/631) | closed-loop `PolicyClient` + state machine | — |
| 1 | [#632](https://github.com/jtrefon/amber/issues/632) | policy zoo and score discrimination | #631 |
| 1 | [#633](https://github.com/jtrefon/amber/issues/633) | workspace, request-contract and fault schema | #630, #631 |
| 2 | [#634](https://github.com/jtrefon/amber/issues/634) | CLI agent journeys (first 5) | #630, #631, #633 |
| 2 | [#645](https://github.com/jtrefon/amber/issues/645) | CLI approval/cancel/session/subagent/compression | #634 |
| 2 | [#635](https://github.com/jtrefon/amber/issues/635) | TUI harness extraction + 3 journeys | #630, #631, #633 |
| 2 | [#636](https://github.com/jtrefon/amber/issues/636) | TUI approval/cancel/session/subagent journeys | #635 |
| 3 | [#637](https://github.com/jtrefon/amber/issues/637) | trace replay | #630 |
| 3 | [#638](https://github.com/jtrefon/amber/issues/638) | trace capture + pinned corpus | #637 |
| 4 | [#639](https://github.com/jtrefon/amber/issues/639) | skills/MCP/plugin coverage | #634, #635 |
| 4 | [#640](https://github.com/jtrefon/amber/issues/640) | long-horizon repository scenarios | #633, #634 |
| 5 | [#641](https://github.com/jtrefon/amber/issues/641) | fault, invariant and seeded fuzz | #630, #631, #637 |
| 6 | [#642](https://github.com/jtrefon/amber/issues/642) | CI gates, baselines, local parity | #634–#638, #641, #645 |
| 6 | [#643](https://github.com/jtrefon/amber/issues/643) | specs and contributor workflow | all |

### Dependency graph

```
#629 plan ──────────────────────────────────────────────┐
#630 endpoint ──► #637 trace ──► #638 corpus            │
   │                  │                                 │
   ├──► #633 schema   └──► #641 faults                  │
   │        │                                           │
#631 policy ─► #632 zoo                                │
   │        └──► #633 ──► #634 CLI ──► #645 CLI+        │
   │                         │         #639 surfaces    │
   │                         │         #640 long-horizon│
   │                         └──► #635 TUI ──► #636     │
   └──────────────────────────────► #642 CI ◄───────────┘
                                      │
                                      └──► #643 docs
```

Parallel work:

- #630 and #631 can start together; #633 joins them.
- #637 can start once #630 lands; #638 follows.
- #634 and #635 can run in parallel once #633 lands; #645 follows #634.
- #641 can start after #630/#631/#637; it does not block the first CLI/TUI
  journeys.
- #642 is the integration point; do not start it until at least one CLI and one
  TUI suite are green.

---

## 7. Definition of done

The epic is done when all of these are true:

- [ ] One shared endpoint is used by bench, CLI E2E and TUI E2E.
- [ ] Six deterministic policy simulators plus the state-machine policy run.
- [ ] The policy matrix shows distinct, expected scores; no policy saturates.
- [ ] At least 20 trace exchanges across OpenAI, Anthropic and Gemini replay.
- [ ] At least 12 CLI journeys and 10 TUI journeys pass through real binaries.
- [ ] At least 20 fault cases and 1,000 seeded policy sequences pass under
      ASan/UBSan.
- [ ] New code meets ≥80% patch line coverage and the branch ratchet.
- [ ] `agent-e2e` and `tui-agent-e2e` are gating, local-parity jobs.
- [ ] A deliberately broken journey and a deliberately wrong expectation both
      turn the gate red.
- [ ] No CI job calls a live model or a non-loopback network.
- [ ] `BENCHMARK.md`, the benchmark specs and the contributor how-to match the
      implementation.
- [ ] `make verify` is clean; new suites pass under `--repeat 2`.

Runtime budgets are set after the first green run and are ceilings, not exact
timings. A suite that cannot fit the budget is split by surface, not weakened.

---

## 8. Non-goals

- A live model in CI, a model leaderboard, or a paid endpoint.
- Replacing the existing unit, probe or hermetic overhead gates.
- A second benchmark harness or engine changes.
- Pixel-perfect TUI screenshot diffing.
- A full SWE-bench-scale corpus.
- Runtime-loadable plugins or the post-1.0 core split.
- Claiming model quality from a simulated policy. The suite tests the harness's
  support for agentic behavior, not whether a particular model will choose
  well.

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| The simulator and the oracle are written by the same author and agree by construction | Policy zoo with deliberately bad policies; independent expectations; mutate an expectation in a test and require red |
| Fixtures overfit to today's prompts | Strict request contracts fail on drift; trace refresh is a reviewed PR; shape mode for prompt edits |
| TUI pty tests flake | `wait_until`, exit-ready, `--repeat 2`, no tight wall bounds, quarantine policy |
| Runtime grows without bound | Per-journey budget, suite split by surface, CI timeout, baseline wall ceiling |
| A green suite is mistaken for model quality | Every report labels the layer "harness behavior"; live quality remains a separate manual measurement |
| Provider wire format changes | Pinned traces fail with a precise diff; update the fixture deliberately |
| Trace capture leaks a secret | Redaction before write; `gitleaks` on the corpus; a test with a fake key |

---

## 10. Model churn policy

- CI never pins a model name or calls a provider.
- A trace records provider/dialect, not "the model we use this year".
- Model changes cannot change a deterministic result. Only a deliberate trace
  refresh can.
- A live run, if used at release time, is a separate manual artifact and never
  a required check. A failed live run is a data point, not a red gate.
- When a provider changes its wire format, the trace replay fails with the
  field that moved; the fix is a reviewed fixture update, not a model swap.

---

## 11. Appendix A — example scenario

```jsonc
{
  "name": "e2e-read-edit-verify",
  "suite": "e2e",
  "platforms": ["linux", "darwin"],
  "setup": {
    "files": {
      "TASK.md": "Make answer.txt contain the word amber.\n",
      "answer.txt": "old\n"
    }
  },
  "prompt": "Read TASK.md, make the change, verify it, then reply done.",
  "policy": {
    "initial": "read",
    "states": {
      "read": {
        "call": {"tool": "read", "args": {"path": "TASK.md"}},
        "next": "write"
      },
      "write": {
        "call": {"tool": "write", "args": {
          "path": "answer.txt",
          "edits": [{"old": "old", "new": "amber"}]
        }},
        "next": "verify"
      },
      "verify": {
        "call": {"tool": "read", "args": {"path": "answer.txt"}},
        "when": {"last_tool_status": "ok"},
        "next": "done"
      },
      "done": {"final": "done"}
    },
    "on_error": {"retry": 1, "next": "read"},
    "max_transitions": 12
  },
  "expected_workspace": {
    "files": {"answer.txt": {"exact": "amber\n"}},
    "absent": ["scratch.tmp"]
  },
  "request_contract": {
    "system_prefix_stable": true,
    "tools_include": ["read", "write"],
    "no_system_after_first": true
  },
  "budget": {"max_steps": 12, "max_wall_ms": 30000}
}
```

## 12. Appendix B — journey acceptance checklist

Every CLI/TUI journey must assert:

- [ ] the prompt was accepted and the turn finished;
- [ ] the expected final files exist with the expected content;
- [ ] unwanted files do not exist;
- [ ] the expected tools ran, with status;
- [ ] no forbidden tool ran;
- [ ] the exit code is the expected one;
- [ ] telemetry (tokens, calls, usage) is present;
- [ ] a failure prints the screen tail or workspace diff;
- [ ] no state escaped the temp `XDG_*`/`AMBER_WORKSPACE` roots.

## 13. Appendix C — review gates for each child PR

- [ ] A red test exists before the green implementation.
- [ ] New code has unit tests and at least one negative test.
- [ ] New public behavior has an integration or E2E test.
- [ ] `make verify-quick` before each push, `make verify` before review.
- [ ] `make coverage` when production lines change; patch ≥80%.
- [ ] `make flake-lint`, `make test-repeat` for timing-sensitive paths.
- [ ] No live model, no external service, no committed secret.
- [ ] Manual E2E steps and observed result in the PR body.
- [ ] The issue's acceptance criteria are all checked.
