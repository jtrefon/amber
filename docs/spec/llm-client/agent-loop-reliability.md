## Spec: Agent Loop Reliability — Chat Port, Hermetic Tests, Retry Policy

### Purpose

Three gaps in the P1 subsystem (the agent loop) block safe iteration:

1. **The loop is untestable.** `Agent` holds a concrete `LLMClient`
   (`include/agent/agent.h:216`), so `run()`, the confirmation exchange, tool
   dispatch, compression triggering, and loop detection have zero hermetic
   tests. The unit suite exercises only `LLMClient::parse_models` (static);
   the e2e suite covers TUI/command-line logic, not the loop. This already
   cost us: the `confirm_turn` use-after-move (moved-from JSON silently
   killing the tool-dispatch path) was found by clang-tidy, not by a test —
   no test *could* drive that path.
2. **Transient-failure resilience is shallow.** The loop retries once on an
   `"[error during …]"` reply (see `error-handling.md`, invariant 3) with no
   backoff, no retryable/non-retryable discrimination, and no cancellation
   during the retry wait.
3. **Unknown context size disables compression entirely.**
   `lib/compressor.cpp:45` — `if (agent_cfg.context_size <= 0) return false;`
   — and `context_size = 0` is the default ("auto"). Any server that fails
   the probe or does not report `n_ctx` gets no compression gate, no gauge,
   and unbounded context growth.

This spec defines the fix: an `LLMClient` port with a scripted fake for
hermetic loop tests, a typed retry policy with exponential backoff, and a
conservative fallback estimate so the compression gate always has a budget.

### Ownership

- **Source files** (target): `include/agent/llm.h` (becomes the port),
  `lib/llm.cpp` + the existing split TUs (`request_builder`, `sse_parser`,
  `http_transport`, `model_probe`, `debug_log`) form `HttpLLMClient`,
  `lib/agent.cpp` (client member + retry), `lib/agent_helpers.cpp`
  (`safe_chat_once` retry policy), `lib/compressor.cpp` (n_ctx fallback)
- **Test files** (target): `tests/fake_llm.h` (new), `tests/agent_loop_test.cpp`
  (new, added to `UNITTEST_OBJ` in `Makefile.in`), `tests/run_tests.cpp`
- **Spec status**: design — implementation tracked in `docs/agent-loop-tracker.md`.

---

### Non-negotiable: context immutability

The `Context` stack (`include/agent/context.h`) is a **pure stack**:
`push()`/`pop()` (LIFO)/`clear()`/`get_all()` are the only mutation surface;
messages are sealed on push; the FNV-1a hash chain asserted in `get_all()`
guards against any in-place mutation. This exists to keep the KV-cache prefill
prefix stable across turns. **This work must not regress it:**

1. **No new `Context` API.** The port extraction, the retry policy, and the
   n_ctx fallback introduce zero changes to `Context`. If a change appears to
   need a context mutation (replace/insert/update), the design is wrong.
2. **Prompt augmentation stays on the copy.** `Agent::chat_once` builds
   `prompt_copy` (a copy of `get_all()`) and injects memory/skill/discovery
   slots there — never into the stack. The retry re-sends the *same*
   `prompt_copy` snapshot, which is byte-identical serialization: the prefix
   is not invalidated by a retry.
3. **Compression stays clear+push.** The only context mutation outside
   `push` is the compression rebuild (`clear()` then `push()` each message) —
   unchanged, and exercised by the new hermetic compression test.
4. **The hash chain is tested, not assumed.** A hermetic test runs a full
   multi-turn `run()` (tool calls + confirmation + compression) and asserts
   `get_all()` returns intact at every step — any future mutation regression
   fails red.

---

### 1. The `LLMClient` port

**Target:**

```cpp
// include/agent/llm.h — the port (current class body becomes the impl)
class LLMClient {
public:
    virtual ~LLMClient() = default;
    virtual ServerInfo probe_server() const = 0;
    virtual Message chat(const std::vector<Message>& messages,
                         const std::vector<Tool*>& tools,
                         Stats* stats = nullptr) = 0;
    virtual Message chat_stream(const std::vector<Message>& messages,
                                const std::vector<Tool*>& tools,
                                const std::function<void(const StreamChunk&)>&,
                                Stats* stats = nullptr) = 0;
};
```

- The current concrete class is renamed `HttpLLMClient` (one new TU boundary
  in `lib/llm.cpp`; the already-split transport TUs are untouched). The
  `Config` member, curl callbacks, and statics (`parse_models`,
  `merge_server_info`, `apply_server_autodetect`) stay where they are —
  `parse_models`/`merge_server_info` become free functions (pure, already
  tested).
- `Agent` gains `std::unique_ptr<LLMClient> client_`; its constructor takes
  an optional client (`= {}` → real one built from `cfg_`). CLI/TUI/smoketest
  call sites unchanged.
- This is a **behavior-identical refactor**: existing tests + the full suite
  must stay green before any new test lands.

**`FakeLLMClient`** (`tests/fake_llm.h`): scripted outcome queue (text reply,
tool_calls, throw retryable / non-retryable), records every request
(messages + tool schemas) for assertions, configurable `prompt_tokens` per
reply to drive the compression gate, and a cancellation-aware delay option.

---

### 2. Retry policy (typed, backoff, cancellable)

| Outcome | Retryable |
|---|---|
| curl/transport failure, timeout, empty body | ✅ yes |
| HTTP 429, 502, 503, 504 | ✅ yes |
| HTTP 400, 401, 403, 404 (auth/misconfig) | ❌ no — fail fast |
| Malformed JSON response | ❌ no (surfaced as error message; the model self-recovers) |

- `ApiError : std::runtime_error { long status; bool retryable; }` thrown by
  the HTTP layer for non-2xx; curl failures remain `std::runtime_error`
  (retryable).
- `chat_with_retry(...)`: up to **3 attempts**, a growing ladder **1s → 2s → 4s
  → 8s (capped)** with **±25 % jitter** derived from the failure text and the
  attempt (`retry_backoff_ms`) — a metronome is the worst shape of traffic for
  an endpoint that is recovering or throttling, and deriving the jitter from the
  failure keeps a given failure's retries reproducible. Sleeps are sliced at
  100 ms **polling the CancellationToken** (Esc aborts between attempts),
  `on_status` per attempt (naming the real wait), and the exhaustion produces a
  `[harness]` notice naming the attempt count. The retried request is the
  identical `prompt_copy` snapshot — no context change, no prefix invalidation.
- **`Retry-After` is not honoured yet.** The transport has no response-header
  seam (adding one widens an already eleven-parameter `curl_exec`), and the
  measured failures this ladder exists for carried none: 59 upstream 503s in
  session `1792189515823-0`, zero 429s, no `Retry-After`, no quota wording.
  Deferred until a provider demonstrates it.
- `safe_chat_once` keeps its contract (never throws, converts to
  `"[error during …]"`); the retry decision moves to the typed error.
  `error-handling.md` invariant 3 is updated accordingly.

---

### 3. n_ctx fallback (the gate always has a budget)

- When `context_size <= 0` (probe failed / server silent), the compression
  gate falls back to a **conservative default budget** (32 000 tokens,
  documented as an estimate) instead of returning `false`.
- The gauge (`context_size > 0`) still hides when unknown; only the gate
  changes. A config-explicit `context_size` keeps winning.
- The fallback constant lives next to the gate with a comment; no new config
  key (YAGNI — the real `n_ctx` from the probe overrides when available).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Real or fake LLM client; typed errors; scripted token counts; unknown context sizes |
| **Output** | Hermetic loop behaviour; retried replies; gated compression |
| **Error states** | Retryable ×3 → error surfaced, conversation intact; non-retryable → immediate; cancel → aborted wait |
| **Invariants** | Below. |
| **Thread safety** | Agent thread only (unchanged). |

### Invariants

1. **Context immutability holds** (see the non-negotiable section): no new
   mutation API, no in-place edits, hash chain asserted by tests.
2. **The port refactor is behavior-identical.** The full existing suite is
   green before any behavioral change lands.
3. **Retry re-sends the identical snapshot.** No context mutation between
   attempts; KV prefix stable.
4. **Retryable ≠ everything.** Auth/config errors never burn retries.
5. **The gate always has a budget.** Unknown `n_ctx` → fallback estimate,
   never "no compression".
6. **The fake is the only test seam.** No test touches the network; the
   transport TUs are never exercised hermetically.

---

### Scenarios

#### [AL-01] Plain reply round trip

- **Given**: fake scripts one text reply
- **Input**: `run("hi")`
- **Expected**: Returns the text in ONE generation — a reply with no tool calls is the turn's end, and nothing is injected behind it (context: system + user + assistant); `get_all()` chain intact.
- **On failure**: Loop misbehaves without a server to hide it.

#### [AL-02] Tool round trip

- **Given**: fake scripts tool_calls → final text; registry has a real tool
- **Input**: `run(...)`
- **Expected**: Tool executed, result fed back as a `tool` message, final text returned; two generations, no third.
- **On failure**: Dispatch broken silently.

#### [AL-03] A reply carrying content AND tool calls dispatches the calls

- **Given**: a reply with both `content` and `tool_calls`
- **Input**: one turn
- **Expected**: `dispatch_tool_calls` runs and the loop continues (the use-after-move regression: moved-from state used to swallow the calls).
- **On failure**: Moved-from state swallows the tool calls.

#### [AL-04] Max tool iterations cap

- **Given**: model calls tools forever
- **Input**: `run(...)`
- **Expected**: Loop stops at `max_tool_iterations` with a bounded reply; the reply is announced to the host and the stop reason is pushed as a `[harness]` notice (a bound-ended turn is part of the record).
- **On failure**: Unbounded loop, or a stop nobody sees.

#### [AL-05] Text loop detection

- **Given**: model repeats an identical narration while calling different tools (the opt-in guard)
- **Input**: `run(...)`
- **Expected**: `[harness]` steer at repeat 2; hard stop after the detection threshold.
- **On failure**: Infinite echo.

#### [AL-06] Compression triggers on scripted tokens

- **Given**: fake reports `prompt_tokens` ≥ threshold; cooldown expired
- **Input**: `run(...)` (multi-turn)
- **Expected**: A compression request observed on the fake; context rebuilt
  via clear+push; `get_all()` intact.
- **On failure**: Gate dead or hash chain broken.

#### [AL-07] Retryable error → backoff → success

- **Given**: fake throws retryable twice, then succeeds
- **Input**: one turn
- **Expected**: 3 attempts observed; final reply ok; the failures stay silent (a recovered hiccup is not the model's business); context has exactly one user turn.
- **On failure**: Double-pushed turns or no retry.

#### [AL-08] Non-retryable error fails fast, then reports

- **Given**: fake throws non-retryable (401-style), then scripts a text reply
- **Input**: one turn
- **Expected**: One attempt only; a `role="user"`, `harness`-flagged notice naming the failure and the attempt count is pushed; the model's next reply ends the turn.
- **On failure**: Backoff burned on auth errors, or the turn dies with the model untold.

#### [AL-09] Retries exhausted

- **Given**: fake throws retryable 3×
- **Input**: one turn
- **Expected**: One `[harness]` notice naming "3 attempts"; the model's own reply follows; no harness text ever carries `role="assistant"`; the prompt is intact for a manual retry.
- **On failure**: Exception escapes `run()`, or the model is left in the dark.

#### [AL-13] A transient self-healed outage is silent

- **Given**: fake throws retryable twice, then answers
- **Input**: one turn
- **Expected**: No `harness` message at all — nothing happened that the model could act on.
- **On failure**: Notice spam for hiccups the retry already handled.

#### [AL-14] A closing marker is part of the answer

- **Given**: the reply ends with `**done.**`
- **Input**: one turn
- **Expected**: The whole reply (marker included) is returned, in one generation; the harness neither strips it nor asks anything after it.
- **On failure**: The old re-injection loop (a finished coverage review became 98 bash calls).

#### [AL-15] An unrecoverable request reports instead of throwing

- **Given**: the adapted retry also fails (template-parser 400 twice per request, two requests)
- **Input**: `run("hi")`
- **Expected**: No exception; two notices (one per exhausted request); the model's reply closes the turn.
- **On failure**: A dead session with no record of why.

#### [AL-10] Cancellation during backoff

- **Given**: fake throws retryable; token requested during the 1s wait
- **Input**: one turn
- **Expected**: Backoff aborted promptly; no further attempts.
- **On failure**: Uninterruptible sleep.

#### [AL-11] Unknown n_ctx engages the fallback gate

- **Given**: `context_size = 0`; scripted prompt tokens over the fallback
  budget
- **Input**: multi-turn `run(...)`
- **Expected**: Compression still fires (fallback estimate).
- **On failure**: Unbounded growth (today's behavior).

#### [AL-16] Silence after tool calls

- **Given**: the model answers a tool result with an empty reply (no text, no calls)
- **Input**: one turn
- **Expected**: One `[harness]` nudge; if silence repeats, the turn stops with the fallback and the stop notice. `chat_calls` shows exactly the rounds spent (no spin to the iteration cap).
- **On failure**: A silent death, or a loop that burns the budget on empty replies.

#### [AL-17] A failing endpoint has its own bound

- **Given**: every request fails every attempt (3 requests × 3 attempts)
- **Input**: `run("hi")`
- **Expected**: Three `[harness]` failure notices, then the bound stops the turn with the fallback and one stop notice — the endpoint, not the model, is the problem.
- **On failure**: Unbounded retry storm, or the model blamed for an outage.

#### [AL-18] The loop's state reaches the model before the bound does

- **Given**: `max_tool_iterations = 5`, the model calls tools each round
- **Input**: one turn
- **Expected**: No request before step 4 mentions the budget; the step-4 request carries one `[harness]` notice naming `step 4/5`. Announced once — a ceiling that arrives without warning is a trap.
- **On failure**: A silent truncation the model cannot react to.

#### [AL-19] A repeat is reported, never reinforced

- **Given**: the model issues the same call three times (detection off)
- **Input**: one turn, then a closing reply
- **Expected**: One `[harness]` note naming the repetition; the turn ends with the model's own reply — nothing intervenes. `canonical_call_key` treats a bash call with a different `timeout` as the same call.
- **On failure**: The old trap: 30 escalations counted as 30 fresh calls.

#### [AL-20] Client notices reach the model

- **Given**: an `LLMClient` that accumulated a fact about the request (a window the server clamped, tool calls it had to drop)
- **Input**: one turn, then a second
- **Expected**: The fact lands in the conversation as a `[harness]` notice and rides the next request. `take_notices()` drains: nothing is reported twice.
- **On failure**: The model works against state it cannot see — the hidden clamp that made a long session unexplainable.

#### [AL-12] Context hash chain survives a full session

- **Given**: multi-turn run with tool calls + compression
- **Input**: the whole scenario set above
- **Expected**: `get_all()` asserts at every step; zero mutation APIs added.
- **On failure**: Any context regression fails red here.

---

### Cross-references

- **Depends on**: `llm-client/error-handling.md` (retry contract updated),
  `compression/compression-pipeline.md` (gate budget), `context.h` (immutability)
- **Depended on by**: `docs/spec/INDEX.md`, `docs/agent-loop-tracker.md`
- **Test coverage**: `tests/agent_loop_test.cpp` (AL-01..12 plus the
  turn-contract and notice scenarios AL-13..15),
  `tests/run_tests.cpp`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (port + fake + hermetic loop tests; typed retry policy; n_ctx fallback; context-immutability constraint) |
| 2026-10-18 | Turn contract: a reply with no tool calls ends the turn (the `confirm_turn` probe is gone); failures reach the model as `[harness]` notices (AL-08, AL-09, AL-13, AL-15); AL-03 re-scoped to the mixed reply |
