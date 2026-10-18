# Hung test suite, the loop it caused, and the upstream 503s behind it

> **Superseded in part (2026-10-18):** the loop-guard design in Sequence step 4 is
> replaced by `agent-loop-contract-and-context-transparency-2026-10-18.md` — the
> harness no longer enforces loops (no forced continuation, no repeat-triggered
> intervention); it bounds and reports state instead. Everything else here still
> stands, and steps 1–3, 5–7 are landed.

Session `1792189515823-0` ("please review test coverage of our project"), continued
2026-10-17 17:20–18:09 on `kilo-auto/free`. The 302 type errors are gone (PR #271) —
this is what is left.

## Problem

Measured from the session file and its transcript log:

- **108 tool calls: 75 of them were spent trying to run or locate the test suite.**
  `run_tests` ×53, `pytest` ×6, `make test` ×5 (64 suite runs), `find -name run_tests` ×9,
  `run_tests --help` ×2. The question ("what is our coverage") was never answered.
- **30 explicit `timeout N` values, escalating:** 30, 5, 120, 180, 300, 120, 300, 600,
  900, 120, **1800**, 1200, 1200, 1800, … max **1800 s in a single bash call**. The model
  read "killed by timeout" as "needs more time", which is the loop the user saw.
- **53 `chat_error` events, 51 of them `HTTP 503` from the upstream**, spread over
  **47 minutes**; the turn ended on a 503 (message 226) and the session stops there.
- Reproduced today: `timeout 45 ./run_tests` → **rc=124 (hang)**, tail of output stops
  after the `agent*` tests. Full runs of the same binary pass (1135/0, three times), so
  the hang is a race, not a missing dependency.

Five defects, in the order they compound:

1. **A deterministic ceiling overrides the idle counter — the loop's engine.** The counter
   is already idle-based (`bash_tool.cpp:59` resets it on output; `lib/job.cpp:147` counts
   `seconds_since_output`), but `hard_timeout_s = 2 * timeout` (`bash_tool.cpp:183`, and the
   `hard_deadline` in `wait_for_job`) counts **from task start** and kills anyway. The suite
   prints a PASS line every few seconds, so silence never occurs and the wall — 120 s at the
   default — decides. The model saw that as an idle timeout, kept raising the *shell* prefix
   (`timeout 300 ./run_tests`), which the tool never reads: 30 escalations that could not work.
2. **A test can hang forever.** `spawn_mock_http` serves exactly one connection; a test
   whose client connects a second time (or whose accept is consumed by a neighbouring
   test) blocks in read/accept with no deadline. There is no per-test watchdog, so the
   whole suite hangs and the only escape is `timeout(1)` from outside.
3. **The harness cannot be interrogated.** `run_tests` has no `--list` and no positional
   filter (`RUN_TESTS_ONLY` is the only way in), so the model could not name, isolate or
   bisect the hanging test. It re-ran the whole suite instead — 65 times.
4. **Retries are invisible to the loop guard.** The per-iteration guard (PR #271) compares
   a call against its predecessor; each retry differed in its `timeout` argument, so it
   never fired. A timeout is also reported as an ordinary failure, so nothing in the
   result says "the command was killed, not answered".
5. **Upstream 503s are surfaced but never retried.** Every one of the 51 cost a recovery
   step and a fresh prompt; the last one ended the turn with the model's work intact but
   unreported.

Found while verifying (small, same area): with `debug on`, a **resumed** session never
opens the journal — `log_.open_errors()` sits inside the `!log_.enabled()` branch of
`log_and_push_user_prompt`, so it runs only for a session's *first* prompt.

## Decision

- **A test may not hang.** The mock serves until the test tears it down; a per-test
  watchdog turns any hang into a named failure, so a hang is red, not silent.
- **The harness becomes interrogable**: `run_tests --list` prints test names one per
  line, a positional argument is a substring filter, and both are documented where the
  agent will read them (`prompts/tools.md`), so isolating a test is one call.
- **A repeat is a decision, not a guess**: the result names how long the process has been
  inactive and the three ways forward, so the guard can count successive *inactivity returns
  on the same command* (the timeout argument stripped) as repeats, intervene after two, and
  stop the turn with the same actionable message rule as internal failures.
- **No ceiling exists, by construction.** The inactivity counter is the only mechanism, and
  the service watches and reports rather than kills. When it fires the tool returns and the
  process keeps running as a registered job; the agent decides — `process_read wait=60` (same
  again), `process_read wait=N` (a different budget, explicit), or `process_stop` (terminate,
  reporting how long the process had been inactive). `JobService::start` already reads a
  non-positive budget as "disabled", so the ceiling is removed rather than raised.
- **The journal opens where it writes**: `record_error` opens the sink lazily under
  `debug`, so a resumed session is covered too. Off stays off — one bool test at entry.
- **One bounded retry for pre-first-token failures** (429/5xx + transport), because a
  47-minute session should not die on a transient upstream blip. Split to its own PR.

## Design

**Watchdog** (`tests/run_tests.cpp`): the runner records the current test name, arms
`SIGALRM` per test (`AMBER_TEST_TIMEOUT_MS`, default 120000 — the slowest test today is
~38 s), and the handler prints `[ TIMEOUT ] <name>` and `_exit(1)`. No thread per test,
no cancellation of hung I/O: the run ends non-zero *and names the test*, which is the
property CI and the agent both need. `0` disables it.

**Mock** (`spawn_mock_http` and friends): accept in a loop until a stop flag, serve each
connection, close the listener and join on teardown. The one-shot accept becomes the
`--once` case the few tests that need it can ask for explicitly.

**Interrogable harness**: `--list` and the positional filter are two lines in `main()`
around the existing name matching (`RUN_TESTS_ONLY` keeps working); the usage line is
printed for `--help`, and `prompts/tools.md` gains the same usage line so the agent
discovers it without burning nine calls on `find`.

**Inactivity returns, it does not kill** (`tools/bash_tool.cpp`, `lib/job.cpp`): delete
`hard_timeout_s = 2 * timeout` and the `hard_deadline` branch; pass the job `0` for both
budgets so the service never kills on its own. `run_with_timeout` stops reading at the idle
budget and returns "still running" with elapsed time, `seconds_since_output` and output size;
the job survives the call, so `/job`, the status bar and `process_read`/`process_stop`
(`tools/process_tool.cpp`) act on a live process. `meta.timeout_kind = "idle"` names what
fired. Reaping keeps its contract: SIGTERM, 2 s grace, SIGKILL to the group, bounded drain
(never "until EOF"), `waitpid` + reader join, no survivors in `ps`.

**Loop signal**: the result carries the inactivity duration and the three options, so a
repeat is a decision rather than a guess. The guard compares a canonical form of the command
(timeout argument stripped) against the previous *timed-out* calls and stops the turn after
two with the message the internal-failure path already uses.

**Journal**: `open_errors` moves out of the first-prompt branch into the two places a
session log is opened (`Agent` construction, session resume), gated on `cfg.debug`; the
sink stays lazy and the off-path stays a single bool test.

**Retry** (separate commit, separable PR): in the transport, an attempt loop that retries
only when **no response body byte has been delivered yet** — 3 attempts, 500 ms ×2 with
jitter — for 429/5xx and transport errors, cancelled by the `CancellationToken`. Once the
first token reaches the callback the request is never retried (the client cannot un-say
what it already streamed), and each attempt is recorded as `chat_retry` in the transcript.

## Sequence (red → green, one commit each)

1. **Watchdog first**, so every later red *fails* instead of hanging: its own test arms a
   deliberate `sleep` test and asserts the runner reports `TIMEOUT`.
2. Mock serves until closed (+ the `--once` case); the compress/window test that hangs
   today becomes the regression test.
3. `--list` + positional filter + `prompts/tools.md` line.
4. Timeout result text + loop guard (unit test on the canonical form and on the streak).
5. Stop-and-ask: the ceiling is deleted, inactivity returns to the agent, `process_read`
   carries the budget (default = the idle budget), `process_stop` reports the inactivity
   duration; reaping contract (SIGTERM → grace → SIGKILL, bounded drain, join, no survivors).
6. Journal open sites (+ test: resumed session with `debug on` writes `errors.jsonl`).
7. Pre-first-token retry (+ test: a mock that 503s once then succeeds; and one that fails
   *after* the first token must not be retried).

## Invariants

- `debug` stays off by default; the off path is one bool test, no file, no formatting.
- No test blocks indefinitely: it completes or is reported as `TIMEOUT`, and the run is
  non-zero. CI's `timeout-minutes` stays as the outer backstop, not the only one.
- A retry never duplicates streamed output, never retries a cancelled request, and never
  changes the number of tool calls the model sees for one turn.
- The suite stays hermetic: no test reaches the network or a live provider.
- Existing green behaviour is untouched: 1135/0 stays 1135/0 plus the new tests.

## Decisions taken (2026-10-17)

1. Watchdog **120 s** per test, and on expiry the run **ends non-zero naming the test** —
   continuing is not safe while the hung thread holds the mock's port.
2. **No ceiling, deterministic or otherwise.** Inactivity is the only trigger and it *returns*
   to the agent, which decides: wait the same budget, wait a different one, or terminate. The
   environment facilitates; it does not jail.
3. Retry for pre-first-token failures is a **separate PR**, after the loop is fixed — it is
   the one change that can mask a failure.
4. The loop guard **stops the turn** on the second successive timeout of the same command,
   with the message the internal-failure path already uses.

## Evidence

```
run_tests invocations ........ 64 suite runs (run_tests 53, pytest 6, make test 5)
                              + 11 locating it (find 9, --help 2) = 75 of 108 calls
explicit timeout values ...... 30 (max 1800 s in one bash call)
chat_error events ............ 53 (51 × HTTP 503), 47 min window
reproduction ................. timeout 45 ./run_tests  -> rc=124, twice
full runs .................... make test -> 1135 passed, 0 failed (3×)
typed-error repro (fixed) .... meta["timeout"]=60; .value("timeout", false) -> type_error.302
```

Transcripts: `~/.local/state/amber/projects/home-jack-Projects-cpp-agent-d1013698/`
(`sessions/1792189515823-0.json`, `logs/1792250408004.jsonl`).
