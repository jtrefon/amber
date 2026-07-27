## Spec: Context Compression Pipeline

### Purpose
Reduce conversation history size to fit within the LLM's context window without
losing essential task state. The pipeline collapses repetitive tool-call loops,
asks the LLM to classify each turn as core/context/prune, replaces archived
turns with a JSON summary block, and optionally extracts memories and skills
from the compressed content.

### Ownership
- **Source files**: `lib/compressor.cpp` (`CompressionPipeline`, `DefaultCompressionGate`, `CompressionReporter`), `lib/compressor_scanner.cpp` (`collapse_loops`), `lib/compressor_request.cpp` (`build_compression_request`), `lib/compressor_parser.cpp` (`parse_compression_response`), `lib/compressor_apply.cpp` (`apply_classification`, `apply_memory_ops`, `apply_skill_ops`, `build_compression_result`), `lib/agent.cpp` (`compress_now()`, gate call in `chat_once()`), `include/agent/compressor.h`
- **Test files**: `tests/run_tests.cpp` — compression tests (lines 1555–1782 + integration 1878–1949)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `std::vector<Message>` (full or partial conversation history) + `CompressionConfig` + `LLMClient` (for the classification LLM call) |
| **Output** | Compressed `std::vector<Message>`: core turns verbatim, pruned turns removed, archived turns replaced by a synthetic `compressed_context` system message. |
| **Error states** | LLM call failure → return input unchanged. Parse failure (invalid JSON) → return post-collapse, pre-classification messages. Empty input → return unchanged. |
| **Invariants** | See below. |
| **Thread safety** | Called synchronously from agent thread. `Agent::compress_now()` replaces `history_` on the calling thread. |

### Invariants

1. The number of messages after compression is never greater than before (monotonic reduction).
2. No message is ever duplicated in the compressed output.
3. The order of remaining `core` messages is preserved (stable).
4. `prune` turns are removed entirely — no trace remains in the output.
5. `context` turns are removed from the main sequence and replaced by an entry in the `archive[]` JSON block.
6. The archive JSON block is inserted as a system message with content `"Compressed conversation context:\n{...}"`.
7. If the LLM call or parse fails, the input history is returned unchanged (safe fallback).
8. The loop-collapse step (consecutive identical tool calls) always runs before the LLM classification step.

---

### Scenarios

#### [CP-01] History below minimum turns — no compression

- **Given**: History with fewer than `cfg_.min_turns` (default 10)
- **Input**: `CompressionGate::should_compress(history, cfg)` with 3 messages
- **Expected**: Returns `false`. Compression not triggered.
- **On failure**: Premature compression of short conversation.

#### [CP-02] Context utilisation below threshold — no compression

- **Given**: History fits in context comfortably (e.g. 20% of context window used)
- **Input**: `CompressionGate::should_compress(history, cfg)` with 1,000/8,192 tokens
- **Expected**: Returns `false`.
- **Threshold logic**: Uses `cfg.prompt_tokens_used / context_size` when real token count is known. Falls back to `total_chars / 3 / context_size` when tokens are not yet known.

#### [CP-03] Context utilisation exceeds threshold — compression triggers

- **Given**: History filling >50% of context window
- **Input**: `CompressionGate::should_compress(history, cfg)` with 5,000/8,192 tokens
- **Expected**: Returns `true`.
- **On failure**: Compression never triggers, context window overflows.

#### [CP-04] Cooldown prevents re-compression

- **Given**: Compression just ran (cooldown = 20 turns)
- **Input**: `compression_gate.is_within_cooldown(turn_counter_)`
- **Expected**: Returns `true` for 20 subsequent turns. Returns `false` at turn 21+.
- **On failure**: Compression fires every turn, thrashing history and wasting tokens.

#### [CP-05] Loop collapse with 3+ identical tool calls

- **Given**: History with 4 consecutive `read(foo.txt)` calls and their results
- **Input**: `collapse_loops(history)`
- **Expected**: The 3rd+ identical call sequences are removed. A single note message inserted: `"[loop collapsed] turns 2-5: tool loop detected, 3 identical calls collapsed"`.
- **On failure**: Garbage results (wrong turns collapsed, note malformed).

#### [CP-06] Loop collapse with short history — no-op

- **Given**: History with <4 messages
- **Input**: `collapse_loops(history)`
- **Expected**: Returns history unchanged.
- **On failure**: Crash or undefined access.

#### [CP-07] Build compression request

- **Given**: Post-collapse history
- **Input**: `build_compression_request(history)`
- **Expected**: Returns a single `user` role message containing instructions for JSON classification output. The message documents three tags (`core`, `context`, `prune`) and memory/skill ops (`upsert`, `deprecate`).
- **Content**: Must not reference any tool or system prompt (plain user message to preserve KV cache).

#### [CP-08] LLM returns valid classification JSON

- **Given**: Valid JSON response from LLM
- **Input**: LLM reply with ```json\n{"classification": [...], "memories": [...], "skills": [...]}\n```
- **Expected**: `parse_compression_response()` strips markdown fences, extracts JSON, parses segments/memories/skills. Returns fully populated `CompressionResponse`.
- **JSON extraction priority**: Direct parse → strip ```json fences → strip \\n → find outermost `{}`.

#### [CP-09] LLM returns non-JSON (parse failure)

- **Given**: LLM returns plain text instead of JSON
- **Input**: LLM says "I don't understand compression"
- **Expected**: `parse_compression_response()` returns empty `CompressionResponse`. Pipeline returns the post-collapse, pre-classification history. No data lost.
- **On failure**: Crash from try/catch exception.
- **Regression guard**: `parse_compression_response_invalid_json` test.

#### [CP-10] Classification: all core

- **Given**: LLM classifies all turns as `core`
- **Input**: `apply_classification(history, response)` where all segments have `tag = "core"`
- **Expected**: All messages preserved verbatim. No archive block inserted. Zero messages removed.
- **Regression guard**: `apply_classification_all_core` test.

#### [CP-11] Classification: prunes and archives

- **Given**: Mixed classification: 2 core, 3 context, 1 prune
- **Input**: 6-turn history; LLM returns segments tagging each turn
- **Expected**: Pruned turn removed. Context turns removed from sequence, replaced by `archive[0]` entry in compressed context block. Core turns preserved verbatim. Output shorter than input.
- **Archive JSON**: Contains `archive[0].turns = "1-3"` and `archive[0].summary`.
- **Regression guard**: `apply_classification_prunes_and_archives` test.

#### [CP-12] Classification: empty response (no-op)

- **Given**: LLM returns `{"classification": []}`
- **Input**: `apply_classification(history, empty_response)`
- **Expected**: Returns history unchanged. All messages preserved.
- **Regression guard**: `apply_classification_empty` test.

#### [CP-13] Memory ops applied after compression

- **Given**: LLM response includes `"memories": [{"name": "proj-name", "content": "Project X", "action": "upsert"}]`
- **Input**: `apply_memory_ops(response, store)`
- **Expected**: New memory created with `name = "proj-name"`, `content = "Project X"`, `evidence_count = 3`, `promoted = true`.
- **On conflict**: If memory with same name but different content exists, op is SKIPPED silently.

#### [CP-14] Skill ops applied after compression

- **Given**: LLM response includes `"skills": [{"name": "deploy-cmd", "content": "use make deploy", "trigger_phrase": "deploy", "action": "upsert"}]`
- **Input**: `apply_skill_ops(response, store)`
- **Expected**: New skill created with `name = "deploy-cmd"`, `trigger_phrase = "deploy"`. Same conflict rules as memories.

#### [CP-15] Deprecate existing memory

- **Given**: An existing memory/skill with content hash matching
- **Input**: `{"action": "deprecate", "name": "old-info"}`
- **Expected**: `evidence_count` decremented (floor 0). Content not deleted.
- **On failure**: Evidence count goes negative.

#### [CP-16] Manual compression (`/compress` command)

- **Given**: TUI sends `/compress` to Agent
- **Input**: `Agent::compress_now()` called
- **Expected**: Snapshots `before = history_`. Runs full pipeline (collapse → request → LLM → parse → apply). Replaces `history_` with compressed result. Applies memory/skill ops. Updates `last_compression_` and `last_extraction_`. Compression status emitted via hooks.
- **On failure**: `history_` unchanged (pipeline returns original on error).

#### [CP-17] Automatic compression in `chat_once()` (non-destructive)

- **Given**: Gate triggers mid-conversation
- **Input**: `chat_once()` compresses the prompt COPY before sending to LLM
- **Expected**: `prompt_msgs` is compressed but `history_` is NOT modified. Gate's `last_compress_turn` is updated. Next LLM call sees compressed context.
- **On failure**: `history_` incorrectly replaced (destructive compression during normal chat).

#### [CP-18] Pipeline fallback on LLM failure

- **Given**: LLM client throws during compression call
- **Input**: `CompressionPipeline::compress()` where `client.chat()` throws
- **Expected**: Exception caught. `on_error` fired. Original `history` returned unchanged.
- **Regression guard**: `compressor_pipeline_fallback_on_no_client` test.

#### [CP-19] Compression after loop detection (error retry)

- **Given**: LLM error, retry succeeds
- **Input**: `run()` error-retry path: first call fails, second call succeeds
- **Expected**: Compression is NOT triggered between retries (the retry path avoids compression to preserve KV cache). Only after the successful LLM call does the gate re-evaluate.
- **Rationale**: Documented in `agent.cpp` line 429 comment.

---

### Cross-references

- **Depends on**: `llm-client/streaming.md` (LLM call for classification), `memory/extraction.md` (memory/skill ops), `memory/memory-store.md`
- **Depended on by**: `agent-loop/core-loop.md` (compression gate in `chat_once()`), `docs/spec/INDEX.md` (compression category)
- **Test coverage**: `tests/run_tests.cpp` — `collapse_loops_*` (3 tests), `parse_compression_response_*` (3 tests), `apply_classification_*` (4 tests), `compression_gate_*` (3 tests), `request_builder_returns_message`, `compressor_pipeline_fallback_on_no_client`, `integration_apply_and_retrieve`, `compression_observer_interface`

### Known gaps

1. **No mock LLM in compression tests** — The pipeline's LLM call step only exists in production; tests verify individual sub-steps in isolation.
2. **`CompressionBudget` not enforced** — The `core`/`archive`/`headroom` fractions are declared but never checked. The safety net described in the design doc (walking backward from oldest core turns) is not implemented.
3. **Token estimation is heuristic** — Fallback `total_chars / 3.0` for token count is an approximation; for pure prose, 4 chars/token is more accurate.
4. **`on_compress_start` tokens_before is always 0** — The pipeline reports 0 to the observer; real estimation happens in `Agent::compress_now()` only.
5. **No `turn_start` clamping** — `apply_classification()` clamps `turn_end` to `history.size() - 1` but does not clamp `turn_start`. Negative or out-of-range start values cause undefined `tags[i]` access.
6. **Memory/skill conflict is name-only** — If the LLM reuses a name with different content, the op is silently skipped. No fallback or retry.
7. **History between `compress_now()` and agent loop** — After manual compression, the compressed history includes a synthetic system message. The agent loop treats it as normal `history_` content; the system prompt at `history_[0]` is preserved separately.
