# amber — Agent Loop Reliability Tracker

- **Status:** 🟢 Complete — AL-IMP-001..005 implemented on `feat/agent-loop-hardening`, all gates green
- **Reference:** `docs/spec/llm-client/agent-loop-reliability.md`
- **Issues register:** `docs/issues.md`

---

## How to Use This Tracker

1. Every task follows the **Red → Proposal → Sign-off → Green → PR** workflow
   (see AGENTS.md). On the branch `feat/agent-loop-hardening`:
   - **Red**: Write a failing test first (scenario IDs below map to the spec),
     commit it so CI shows the failure.
   - **Green**: Implement; make the test pass; zero lint/analyze findings.
   - **PR**: Open/update. All checks must pass.
2. **Context immutability is non-negotiable** (spec, "Non-negotiable"
   section): no new `Context` mutation API; prompt augmentation stays on the
   prompt copy; the FNV-1a hash chain is asserted by tests, not assumed.
3. Verification before marking a task `[done]`:
   `make clean && make && make test && make lint && make analyze`.

## Legend

```
[ ] — Not started   [~] — In progress   [x] — Done, all checks pass   [!] — Blocked
```

---

## Task 1: LLMClient port extraction (AL-IMP-001)

| Field | Value |
|---|---|
| **ID** | `AL-IMP-001` |
| **Severity** | 🟠 High |
| **Depends on** | None |
| **Blocks** | AL-IMP-002 (fake needs the port) |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `include/agent/llm.h`, `lib/llm.cpp` (+ split TUs unchanged), `include/agent/agent.h`, `lib/agent.cpp`, `src/main.cpp`, `tui/tui_main.cpp`, `src/smoketest.cpp`, `tools/` (LLMClient call sites, if any) |
| **Spec refs** | `llm-client/agent-loop-reliability.md` §1, [AL-12] |

### Problem

`Agent` holds a concrete `LLMClient`; the loop cannot be driven by a fake.

### Target Architecture

- `LLMClient` becomes a pure-virtual port (`probe_server`, `chat`,
  `chat_stream`). The current class is renamed `HttpLLMClient`; its `Config`
  member and the split TUs (`request_builder`, `sse_parser`, `http_transport`,
  `model_probe`, `debug_log`) are untouched. `parse_models` /
  `merge_server_info` / `apply_server_autodetect` become free functions.
- `Agent` holds `std::unique_ptr<LLMClient>`; the ctor takes an optional
  client (default = real one from `cfg_`). Host call sites unchanged.
- **Behavior-identical refactor**: the full existing suite must be green
  before this task is marked done.

### Refactor Rules

- Zero behavior change; zero `Context` changes.
- No `virtual` on anything that does not belong to the port surface.

### Verification

- [x] `make clean && make && make test && make lint && make analyze` green
      with the port in place and the real client wired
- [x] No diff in `include/agent/context.h`; `git diff` on `lib/agent.cpp`
      shows only the client-member wiring

---

## Task 2: Fake client + hermetic loop tests (AL-IMP-002)

| Field | Value |
|---|---|
| **ID** | `AL-IMP-002` |
| **Severity** | 🟠 High |
| **Depends on** | AL-IMP-001 |
| **Blocks** | AL-IMP-003 (retry tests use the fake) |
| **Estimated effort** | 6-8 hours |
| **Files touched** | `tests/fake_llm.h` (new), `tests/agent_loop_test.cpp` (new), `Makefile.in` (UNITTEST_OBJ) |
| **Spec refs** | `llm-client/agent-loop-reliability.md` §1, [AL-01]–[AL-06], [AL-12] |

### Problem

The P1 loop has no hermetic coverage (the `confirm_turn` use-after-move
shipped because no test could drive the path).

### Target Architecture

- `FakeLLMClient`: scripted outcome queue (text / tool_calls / throw),
  request recording, per-reply `prompt_tokens` (drives the gate), optional
  delay honoring the CancellationToken.
- `tests/agent_loop_test.cpp`: [AL-01] plain reply, [AL-02] tool round trip
  via a real registry tool, [AL-03] **confirmation-probe dispatch regression**,
  [AL-04] max-tool-iterations cap, [AL-05] text-loop detection, [AL-06]
  compression trigger + clear+push rebuild, [AL-12] hash chain intact across
  a full multi-turn run.

### Refactor Rules

- Tests use the real `Agent` + real `Context` + real registry; only the
  client is faked. No network, ever.
- The hash-chain assertion comes from `get_all()` itself (no test-only
  bypass).

### Verification

- [x] Red first: `[AL-03]` fails against the current concrete-client build
      (cannot compile a fake in) — the red is the missing port; after the
      port lands, each scenario test is added and must pass
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 3: Typed retry policy (AL-IMP-003)

| Field | Value |
|---|---|
| **ID** | `AL-IMP-003` |
| **Severity** | 🟠 High |
| **Depends on** | AL-IMP-002 (tests on the fake) |
| **Blocks** | Nothing |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `include/agent/llm.h` (`ApiError`), `lib/http_transport.cpp` (throw typed), `lib/agent_helpers.cpp` (`chat_with_retry`), `lib/agent.cpp`, `docs/spec/llm-client/error-handling.md` (invariant 3) |
| **Spec refs** | `llm-client/agent-loop-reliability.md` §2, [AL-07]–[AL-10] |

### Problem

Single retry, no backoff, no discrimination, no cancellation during the wait.

### Target Architecture

- `ApiError : std::runtime_error { long status; bool retryable; }`; curl
  failures remain retryable `std::runtime_error`.
- `chat_with_retry`: 3 attempts, 1s→2s backoff + jitter, 100 ms-sliced
  sleeps polling the CancellationToken, `on_status` per attempt, existing
  degradation after exhaustion. Retried request = identical snapshot.
- `safe_chat_once` contract unchanged (never throws).

### Refactor Rules

- No context mutation anywhere in the retry path.

### Verification

- [x] Tests [AL-07] backoff→success (attempt counts observed), [AL-08]
      non-retryable fails fast, [AL-09] exhaustion degrades gracefully with
      conversation intact, [AL-10] cancellation aborts the backoff promptly
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 4: n_ctx fallback budget (AL-IMP-004)

| Field | Value |
|---|---|
| **ID** | `AL-IMP-004` |
| **Severity** | 🟡 Medium |
| **Depends on** | AL-IMP-002 (gate test harness) |
| **Blocks** | Nothing |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `lib/compressor.cpp`, `lib/compressor_scanner.cpp` (if the fallback affects estimates), `docs/spec/compression/compression-pipeline.md` |
| **Spec refs** | `llm-client/agent-loop-reliability.md` §3, [AL-11] |

### Problem

`context_size <= 0` disables the gate entirely (unbounded growth on servers
that don't report n_ctx).

### Target Architecture

- Gate falls back to a conservative default budget (32 000 tokens,
  documented) when `context_size <= 0`, instead of returning `false`.
- Explicit config `context_size` and probed values keep winning. The gauge
  still hides when unknown.

### Refactor Rules

- One constant, next to the gate, commented; no new config key.

### Verification

- [x] Test [AL-11]: unknown n_ctx + scripted tokens over the fallback →
      compression fires
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 5: Docs + final gate (AL-IMP-005)

| Field | Value |
|---|---|
| **ID** | `AL-IMP-005` |
| **Severity** | 🟡 Medium |
| **Depends on** | AL-IMP-003, AL-IMP-004 |
| **Blocks** | Nothing |
| **Estimated effort** | 1-2 hours |
| **Files touched** | `docs/spec/INDEX.md`, `docs/spec/MISSION.md` (gap register), `docs/spec/llm-client/error-handling.md` |
| **Spec refs** | — |

### Verification

- [x] INDEX lists the new spec; error-handling.md invariant 3 reflects the
      new policy; MISSION gap register updated ("agent loop untested")
- [x] Final: `make clean && make && make test && make lint && make analyze`
      clean; tracker closed

---

## Implementation notes (findings from the hermetic suite)

- **Text-loop hard stop was unreachable.** The recovery steer cleared
  `last_text`, so `text_loop_count` cycled 0-2 and the `>= 5` hard stop never
  fired — a stuck model burned the full iteration budget. Fixed: the steer no
  longer resets the counter ([AL-05] now hard-stops at repeat 6).
- **Cooldown defaults are sticky.** `compression_min_turns` /
  `compression_cooldown_turns = 0` keep the defaults (10 / 20 turns); you
  cannot disable cooldown via config. The compression tests warm up 21 turns
  to open the gate window.
- **Compression = two LLM calls** (classify + extract), sharing the KV prefix;
  both are scripted in the hermetic tests.

## Dependency Graph

```
AL-IMP-001 (port)
   └── AL-IMP-002 (fake + loop tests)
          ├── AL-IMP-003 (retry policy)
          └── AL-IMP-004 (n_ctx fallback)
                 └── AL-IMP-005 (docs + gate)
```

## Scenario → task map

| Scenario | Task |
|---|---|
| [AL-01] plain reply | AL-IMP-002 |
| [AL-02] tool round trip | AL-IMP-002 |
| [AL-03] confirmation dispatch (regression) | AL-IMP-002 |
| [AL-04] max tool iterations | AL-IMP-002 |
| [AL-05] text loop detection | AL-IMP-002 |
| [AL-06] compression trigger | AL-IMP-002 |
| [AL-07] retry → success | AL-IMP-003 |
| [AL-08] non-retryable fail-fast | AL-IMP-003 |
| [AL-09] exhaustion degrades | AL-IMP-003 |
| [AL-10] cancellation during backoff | AL-IMP-003 |
| [AL-11] n_ctx fallback | AL-IMP-004 |
| [AL-12] context hash chain intact | AL-IMP-002 (and every task) |
