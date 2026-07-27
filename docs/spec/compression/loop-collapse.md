## Spec: Compression — Loop Collapse

### Purpose
Detect and remove consecutive identical tool-call sequences (3+ repetitions of
the same tool with the same arguments) from the conversation history before
the LLM classification pass. This reduces noise and token waste from tool-loop
behaviour.

### Ownership
- **Source files**: `lib/compressor_scanner.cpp` (`collapse_loops()`, 83 lines), `lib/agent_helpers.cpp` (`fingerprint_tool_calls()`)
- **Test files**: `tests/run_tests.cpp` — 3 collapse tests (lines 1555–1609)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `std::vector<Message>` (conversation history) |
| **Output** | Modified vector with loop sequences removed and a `[loop collapsed]` note inserted |
| **Error states** | History <4 messages → no-op (return unchanged). |
| **Invariants** | See below. |
| **Thread safety** | Called synchronously from compression pipeline. |

### Invariants

1. Only consecutive identical `(tool_name, args fingerprint)` sequences of 3+ count as a loop.
2. The fingerprint includes the exact function name and serialised arguments.
3. All messages in the loop range (assistant and tool results) are removed.
4. A single `[loop collapsed]` note message replaces the removed range.
5. Trailing tool/assistant messages that follow the loop are also removed (they depend on the removed loop context).

---

### Scenarios

#### [LC-01] Three identical tool calls → collapsed

- **Given**: History with 3 consecutive `read(foo.txt)` → result → `read(foo.txt)` → result → `read(foo.txt)` → result
- **Input**: `collapse_loops(history)`
- **Expected**: All 6 messages removed. One note inserted: `"[loop collapsed] turns 1-6: tool loop detected, 3 identical calls collapsed"`.
- **Regression guard**: `collapse_loops_removes_tool_loop` test.

#### [LC-02] Two identical tool calls → no-op

- **Given**: Only 2 consecutive identical calls
- **Input**: `collapse_loops(history)`
- **Expected**: History unchanged. Threshold 3 not reached.
- **On failure**: False positive collapse.

#### [LC-03] Short history (<4 messages) → no-op

- **Given**: 3 messages
- **Input**: `collapse_loops(history)`
- **Expected**: Returns immediately. History unchanged.
- **Regression guard**: `collapse_loops_noop_on_short_history` test.

#### [LC-04] No loop detected → no-op

- **Given**: History with different tool calls
- **Input**: `collapse_loops(history)`
- **Expected**: History unchanged.
- **Regression guard**: `collapse_loops_noop_on_no_loop` test.

#### [LC-05] Trailing messages after loop removed

- **Given**: Loop of 3 identical calls, followed by a text-only assistant message
- **Input**: `collapse_loops(history)`
- **Expected**: Loop messages removed. The text-only assistant message after the loop is also removed (depends on removed context).
- **On failure**: Orphaned trailing message preserved.

---

### Cross-references

- **Depends on**: `compression/compression-pipeline.md` (called as first step)
- **Depended on by**: `agent-loop/error-recovery.md` (tool-loop detection is separate from compression loop collapse)
- **Test coverage**: `tests/run_tests.cpp`: `collapse_loops_noop_on_short_history` (1555), `collapse_loops_noop_on_no_loop` (1565), `collapse_loops_removes_tool_loop` (1579)

### Known gaps

1. **Brittle trailing message removal** — The logic removes ANY trailing tool/assistant messages after the loop, which may consume legitimate content that doesn't depend on the loop.
2. **Fingerprint includes full arguments** — Even trivial differences (different whitespace) produce different fingerprints. Only exact argument-equivalent calls are detected.
