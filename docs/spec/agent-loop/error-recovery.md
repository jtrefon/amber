## Spec: Error Recovery (Loop Detection & FailStreak)

### Purpose
Detect and recover from three failure modes in the agent loop: repeated
identical tool calls (tool loop), repeated identical text replies (text loop),
and consecutive tool failures (FailStreak). Recovery injects steering messages
into history or hard-stops with a descriptive fallback.

### Ownership
- **Source files**: `lib/agent.cpp` (`dispatch_with_loop_detection()`, `detect_text_loop()`), `lib/tool_recovery.cpp` (`FailStreak`, `inject_tool_recovery_steer()`), `include/agent/tool_recovery.h`
- **Test files**: `tests/run_tests.cpp` — `agent_stops_on_repeated_empty_arg_tool_call` (lines 1214–1278)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Tool call fingerprints (name+args) or reply text, evaluated each iteration |
| **Output** | Either continue with injected steer, or hard-stop with `[loop detected]` / `[stopped: tool calls kept failing]` |
| **Error states** | None (no exceptions). Detection is best-effort heuristic. |
| **Invariants** | See below. |
| **Thread safety** | Called from agent thread only. |

### Invariants

1. Tool loop detection: 3+ consecutive identical call sets → hard stop.
2. Text loop detection: first repeat (count=2) → inject steer; 5th repeat → hard stop.
3. FailStreak: 3 consecutive `ok=false` → inject recovery steer; 6 consecutive (`ok=false` after steer) → hard stop.
4. Steer messages are pushed to `history_` so the LLM sees them on the next turn.
5. After a steer injection, the counter for that detection type resets.
6. When all detectors are disabled (`cfg.detection_loop = false`), the loop continues until `max_tool_iterations`.

---

### Scenarios

#### [ER-01] Tool loop — 3 identical calls → hard stop

- **Given**: `detection_loop = true`, LLM calls `read(foo.txt)` 3 turns in a row
- **Input**: Turn 1: read(foo). Turn 2: read(foo). Turn 3: read(foo).
- **Expected**: After 3rd call, `dispatch_with_loop_detection()` compares fingerprint → count reaches 3 → hard stop with `"[loop detected: the model called tool(s) 'read' 3 times with the same arguments]"`. No further LLM calls.
- **Regression guard**: `agent_stops_on_repeated_empty_arg_tool_call` test.

#### [ER-02] Tool loop — identical but with different args → no detection

- **Given**: `detection_loop = true`
- **Input**: Turn 1: `read(a)`. Turn 2: `read(b)`. Turn 3: `read(a)`.
- **Expected**: No loop detected (fingerprints differ each time). Loop continues normally.
- **Rationale**: Only exact (name, args) matches count.

#### [ER-03] Tool loop — detection disabled → runs to max iterations

- **Given**: `detection_loop = false`
- **Input**: LLM repeats same tool call 100 times
- **Expected**: No detection. Hits `max_tool_iterations` → `empty_turn_reply()`.
- **On failure**: Premature loop detection when disabled.

#### [ER-04] Text loop — first repeat injects steer

- **Given**: `detection_loop = true`, model returns same text 2 turns in a row
- **Input**: Turn 1: "I need to think about this." Turn 2: "I need to think about this."
- **Expected**: At count=2: steer message injected: `"You are repeating yourself. If you need more information, use a tool. Otherwise respond with your final answer."`. `last_text` cleared. Counter resets.
- **On failure**: No steer injected, or steer not visible to LLM.

#### [ER-05] Text loop — 5 identical replies → hard stop

- **Given**: Model keeps repeating after steer injection
- **Input**: 5 identical text replies
- **Expected**: Hard stop: `"[loop detected: the model repeated itself 5 times]"`. Loop breaks, fallback returned.
- **On failure**: Infinite loop without hard stop.

#### [ER-06] Text loop — steer works, counter resets

- **Given**: Count=2, steer injected
- **Input**: Model changes text to something different
- **Expected**: `last_text` reset to new text. Count goes back to 1. Normal flow resumes.
- **On failure**: Stale counter causes premature hard stop.

#### [ER-07] FailStreak — 3 consecutive failures → recovery steer

- **Given**: `detection_loop = true`, tool returns `ok=false` 3 turns in a row
- **Input**: 3 consecutive failed tool batches
- **Expected**: `fail_streak.update()` reaches 3 → `inject_tool_recovery_steer()` called. Steer message: `"I notice the tool calls are failing. Check that paths exist, arguments are valid, and the command is correct before retrying."`.
- **On failure**: No steer injected, model keeps failing indefinitely.

#### [ER-08] FailStreak — 6 consecutive failures (2 attempts) → hard stop

- **Given**: Recovery steer already injected, but model still fails
- **Input**: 3 failures after steer → total 6
- **Expected**: `tool_recovery_attempts >= 1 && fail_streak.count() >= 3` → hard stop: `"[stopped: tool calls kept failing after N attempts]"`.
- **On failure**: No hard stop, infinite loop.

#### [ER-09] FailStreak — recovery works after steer

- **Given**: Recovery steer injected, count reset
- **Input**: Model changes approach, call succeeds
- **Expected**: `fail_streak.reset()` called (dispatcher returns `all_ok = true`). Counter back to 0.
- **On failure**: Stale count causes premature recovery attempt.

#### [ER-10] Empty turn fallback

- **Given**: Loop exits with no final reply (max iterations, hard stop, or loop break with no text)
- **Input**: Any scenario exhausting the loop
- **Expected**: `finish_turn("")` → `empty_turn_reply()`. If history has tool results: `"[agent stopped: the model stopped producing usable output after tool calls; see the ERROR messages above]"`. If no tools: `"[agent stopped: the model produced no usable response]"`.
- **Invariant**: `run()` never returns empty string.

#### [ER-11] LLM error — retry once

- **Given**: `safe_chat_once` catches exception
- **Input**: Network error, server timeout, cancel
- **Expected**: Reply starts with `"[error during"` → retry once. If retry fails too, continue with error message pushed to history. Loop does not abort.
- **Rationale**: Retry before compression preserves KV cache.

---

### Cross-references

- **Depends on**: `agent-loop/core-loop.md`, `agent-loop/tool-dispatch.md`
- **Depended on by**: `docs/spec/INDEX.md` (agent-loop category)
- **Test coverage**: `agent_stops_on_repeated_empty_arg_tool_call` (lines 1214–1278)

### Known gaps

1. **No tests for text loop detection** — `detect_text_loop()` has no direct test coverage.
2. **No tests for FailStreak** — `FailStreak` and `inject_tool_recovery_steer` have no direct test coverage.
3. **No tests for error retry path** — `safe_chat_once` exception → retry path not tested.
4. **No tests for `empty_turn_fallback`** — The fallback message generation is untested.
