## Spec: Context Compression Pipeline

### Purpose

Reduce conversation history size without triggering expensive context reloads. The pipeline runs as a sequence of small LLM requests — each one appends an instruction to the TAIL of the existing conversation, extending the server's KV cache incrementally. No full prefill is triggered during compression. Only after the compressed context is assembled does the next turn pay for one prefill — of a much smaller context.

### The Core Constraint: Prefill Cost

On local llama.cpp inference (4090, 27B Q4_K_M, 262K context):
- **Full prefill** (building KV from scratch for 262K tokens): 10-30 minutes
- **KV extension** (appending 500 tokens to existing KV): ~1-2 seconds

The entire architecture is designed around **zero full prefills during compression**. Every instruction is appended to the existing context as a new user message. The KV cache for the conversation prefix is reused. Only the new instruction tokens and their generated response compute attention.

### Ownership

- **Source files**: `lib/compressor.cpp` (`CompressionPipeline`, `DefaultCompressionGate`, `CompressionReporter`), `lib/compressor_scanner.cpp` (`collapse_loops`), `lib/compressor_request.cpp` (`build_classify_request`, `build_extract_request`, `build_archive_request`), `lib/compressor_parser.cpp` (`parse_classify_response`, `parse_extract_response`), `lib/compressor_apply.cpp` (`apply_classification`, `apply_memory_ops`, `apply_skill_ops`), `lib/agent.cpp` (`compress_now()`, gate call in `chat_once()`), `include/agent/compressor.h`
- **Test files**: `tests/run_tests.cpp` — compression tests

---

### Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Compression cycle                                 │
│                                                                     │
│  Existing KV cache (unchanged prefix, built once):                   │
│  [system][conversation turns 1..N]  ← 262K of KV, paid once         │
│                                                                     │
│  Step 1 — classify turns (push onto live context, LLM, pop)          │
│  context_.push(CLASSIFY_REQ) → LLM → context_.pop()                  │
│  The push/pop pair extends the KV cache from the live prefix.        │
│  ← response: classification_json                                     │
│                                                                     │
│  Step 2 — extract memories/skills (push onto SAME context, LLM, pop)│
│  context_.push(EXTRACT_REQ) → LLM → context_.pop()                   │
│  Same prefix as step 1 — KV cache untouched by the pop.             │
│  ← response: extraction_json (memories + skills)                     │
│                                                                     │
│  Step 3 — assemble compressed context (C++ side, no LLM call)       │
│  Apply classification to snapshot.  Then:                            │
│  context_.clear();                                                   │
│  for (auto& m : compressed) context_.push(std::move(m));             │
│  emit_context_event(context_events_, context_);                      │
│                                                                     │
│  Step 4 — next LLM turn (one prefill of compressed)                  │
│  [system][compressed 2K-10K]  ← cheap prefill, ~1s                  │
│                                                                     │
│  Total: 1 expensive prefill + 2 cheap extensions + 1 cheap prefill  │
└─────────────────────────────────────────────────────────────────────┘
```

**Key property:** Both LLM calls push/pop on the SAME live context. The first call extends the KV cache from the conversation prefix. After the first pop restores the context, the second push extends from the same prefix again — no full prefill between them. The final clear+push is a pure stack rebuild (no mutation, just unstack + restack).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `std::vector<Message>` (full conversation history) + `CompressionConfig` + `LLMClient` |
| **Output** | Compressed `std::vector<Message>`: core turns verbatim, pruned turns removed, archived turns replaced by compressed context message |
| **Error states** | LLM call at any step fails → return input unchanged. Parse failure → return pre-classification history. Empty input → return unchanged. |
| **Thread safety** | Called synchronously from agent thread. `Agent::compress_now()` clears and repushes `context_` on the calling thread via `clear() + push()`. |

### Invariants

1. The number of messages after compression is never greater than before (monotonic reduction).
2. No message is ever duplicated in the compressed output.
3. The order of remaining core messages is preserved (stable).
4. Prune turns are removed entirely — no trace remains in the output.
5. Context turns are removed from the main sequence and replaced by an entry in the archive JSON block.
6. Every LLM request during compression appends as a plain user message — system prompt NEVER changes.
7. If any LLM call or parse fails, the input history is returned unchanged (safe fallback).
8. Loop collapse always runs before the first LLM call (it's C++ side, no token cost).
9. Memory injection does NOT modify the system prompt — it's a separate message slot.

---

### Step-by-Step

#### Step 0: Loop collapse (C++ side, zero LLM calls)

```cpp
void collapse_loops(std::vector<Message>& history);
```

Scans for consecutive identical tool calls (3+ repetitions of same name + arguments). Replaces them with a single `[loop collapsed]` note. Runs on the C++ side before any LLM call — no tokens, no KV cost.

#### Step 1: Classify turns (one LLM call, ~400 tokens of new KV)

Build a user message instructing the LLM to classify each turn range:

```
Request:
  role: "user"
  content: "
Analyze the conversation above. Classify every turn range:

  core    = keep verbatim. Active task, current investigation, decisions.
  context = archive with one-line summary. Useful background.
  prune   = drop entirely. Completed investigations, dead ends,
            irrelevant file reads, loops.

Respond with ONLY a JSON array:
  [{\"turns\": \"0-5\", \"tag\": \"prune\", \"summary\": \"\"},
   {\"turns\": \"6-8\", \"tag\": \"core\", \"summary\": \"\"},
   ...]

Continue the conversation naturally after the JSON.
"

Response:
  [{"turns": "0-5", "tag": "prune", "summary": ""},
   {"turns": "6-8", "tag": "core", "summary": ""},
   ...]
  ...model continues with its normal response if the user message was part
  of the regular turn flow...
```

**Appended to the tail of the existing conversation.** KV for `[system][conv 1..N]` is already cached. Only the ~400 instruction tokens compute attention.

#### Step 2: Extract memories and skills (separate LLM call, same prefix)

```
Request:
  role: "user"
  content: "
Given the classification above, extract durable knowledge.

MEMORIES = facts about the codebase, project, or user.
  Fixed commit messages, config locations, bug causes.
  name: kebab-case unique identifier
  content: one sentence describing the fact

SKILLS = reusable procedures.
  How to run tests, how to deploy, how to reproduce a bug.
  name: kebab-case unique identifier
  trigger_phrase: what the user says to trigger this
  content: one sentence describing the procedure

Respond with ONLY JSON:
  {\"memories\": [
    {\"name\": \"bug-fix-parser-null\", \"content\": \"Parser segfault on empty
     input fixed by adding null check in parse_body()\", \"tags\": [\"parser\"],
     \"action\": \"upsert\"}
  ], \"skills\": [
    {\"name\": \"run-tests\", \"content\": \"make test runs 150+ unit tests\",
     \"trigger_phrase\": \"test\", \"action\": \"upsert\"}
  ]}
"

Response:
  {"memories": [...], "skills": [...]}
```

**Appended AFTER step 1's response.** KV for the prefix + step 1 is already cached. Only step 2's request/response tokens are new.

#### Step 3: Assemble compressed context (C++ side, zero LLM calls)

Using the classification from step 1 and the extraction from step 2:

```cpp
// 1. Apply classification to the pre-classification snapshot.
auto compressed = apply_classification(snapshot, classify_response);
compressed = enforce_headroom(std::move(compressed), context_size);

**Unknown context budget:** when `context_size <= 0` (the server never
reported `n_ctx`), the gate falls back to a conservative 32 000-token budget
(`kFallbackContextBudget` in `lib/compressor.cpp`) so compression still fires
instead of being disabled entirely. Explicit config values and probed `n_ctx`
always win. See `llm-client/agent-loop-reliability.md` [AL-11].

// 2. Apply memory/skill ops to the store.
apply_compression_result(extract_response);

// 3. Rebuild the context stack — pure clear + push (no mutation).
context_.clear();
for (auto& m : compressed)
    context_.push(std::move(m));
emit_context_event(context_events_, context_);
```

The next LLM call builds KV for the compressed context (2K-10K tokens instead of 262K).

---

### Why Multiple Requests Instead of One Big Combined Request

| Approach | KV cost | Parse complexity | Error handling |
|----------|---------|-----------------|----------------|
| **One combined request** (classify + extract + archive in one JSON) | 1 extension of ~500 tokens | Complex JSON with 3 schemas. Hard to test. | All-or-nothing. One bad schema kills all. |
| **Multiple focused requests** (each step is a simple request) | 3 extensions of ~1500 tokens total | Each step is one simple JSON schema. Easy to test. | Per-step. If extraction fails, classification result is still usable. |

**The total KV extension cost is nearly identical** (~500 vs ~1500 tokens of new KV). On a 262K context, the difference between .2% and .6% extension is negligible. The benefit of simpler parsing, easier testing, and per-step error handling far outweighs the trivial token difference.

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

#### [CP-03] Context utilisation exceeds threshold — compression triggers

- **Given**: History filling >50% of context window
- **Input**: `CompressionGate::should_compress(history, cfg)` with 5,000/8,192 tokens
- **Expected**: Returns `true`.
- **On failure**: Compression never triggers, context window overflows.

#### [CP-04] Cooldown prevents re-compression

- **Given**: Compression just ran (cooldown = 20 turns)
- **Input**: `compression_gate.is_within_cooldown(turn_counter_)` — checked in `should_compress()`
- **Expected**: Returns `true` for 20 subsequent turns. Returns `false` at turn 21+.

#### [CP-05] Loop collapse with 3+ identical tool calls (C++ side, free)

- **Given**: History with 4 consecutive `read(foo.txt)` calls and their results
- **Input**: `collapse_loops(history)` — runs before any LLM call
- **Expected**: Loop detected. Repeated calls removed. Note inserted. Zero KV cost.

#### [CP-06] Build classification request

- **Given**: Post-collapse history
- **Input**: `build_classify_request(history)`
- **Expected**: Returns a single `user` role message with classification instructions.
- **Content**: Must not reference tools or modify system prompt. Plain user message to preserve KV prefix.

#### [CP-07] Step 1 — classify turns (appended to tail)

- **Given**: KV cache for [system][conv 1..N] is built. Context at 50% utilisation.
- **Input**: Classify request appended as user message
- **Expected**: KV extends by ~400 tokens. No prefill triggered. Response parsed as JSON array of `{turns, tag, summary}`.
- **On failure**: LLM returns non-JSON → step 1 parse fails → pipeline returns original history unchanged.

#### [CP-08] Step 2 — extract memories/skills (appended after step 1)

- **Given**: Step 1 completed successfully. KV cache now covers: [system][conv 1..N][CLASSIFY_REQ][CLASSIFY_RESP]
- **Input**: Extract request appended after step 1's response
- **Expected**: KV extends by ~300 tokens. No prefill triggered. Response parsed as `{memories: [...], skills: [...]}`.
- **On failure**: Extraction step fails → classification result is still used. No memory ops applied. Degraded but safe.

#### [CP-09] Step 3 — assemble and rebuild (C++ side, zero LLM calls)

- **Given**: Classification from step 1, extraction from step 2
- **Input**: `apply_classification(history, classify_response)` + `apply_compression_result(extract_response)`
- **Expected**: Core turns kept verbatim. Prune turns removed. Context turns archived. Memories/skills applied to store. Decay runs. Store saved. Context rebuilt via clear() + push().
- **Minimum context invariant**: The last user message is ALWAYS preserved, even if the LLM prunes everything.

#### [CP-10] Step 4 — next LLM turn (one cheap prefill)

- **Given**: Context replaced with compressed version (2K-10K tokens)
- **Input**: Normal agent LLM call
- **Expected**: One prefill of the compressed context. ~1 second vs 10-30 minutes for the original 262K.
- **KV cost**: The prefill is unavoidable — the context changed. But the compressed context is 10-50x smaller.

#### [CP-11] Classification: all core — no change

- **Given**: LLM classifies all turns as core
- **Input**: `apply_classification(history, all_core_response)`
- **Expected**: All messages preserved verbatim. Zero messages removed. Archive block NOT inserted.
- **Rationale**: If everything is active work, nothing is compressed. The gate's threshold ensures we don't waste cycles compressing when the context has room.

#### [CP-12] Classification: prunes and archives

- **Given**: Mixed classification: 2 core, 3 context, 1 prune
- **Input**: `apply_classification(history, mixed_response)`
- **Expected**: Pruned turn removed. Context turns removed, replaced by archive entry. Core preserved verbatim. Output shorter than input.

#### [CP-13] Memory ops applied after extraction step

- **Given**: Step 2 returned `"memories":[{"name":"proj-name","content":"Project X"}]`
- **Input**: `apply_memory_ops(store, extract_response.memories, path)`
- **Expected**: New memory created with `evidence_count = promote_threshold`, `promoted = true`. Saved to store.

#### [CP-14] Skill ops applied after extraction step

- **Given**: Step 2 returned `"skills":[{"name":"deploy-cmd","content":"make deploy","trigger_phrase":"deploy"}]`
- **Input**: `apply_skill_ops(store, extract_response.skills, path)`
- **Expected**: New skill created with `trigger_phrase = "deploy"`. Same evidence rules as memories.

#### [CP-15] Step failure — safe fallback at every step

- **Given**: Step 1 LLM call throws (timeout, server error)
- **Input**: Exception in `client.chat()`
- **Expected**: Exception caught. `on_error` fired. Original history returned unchanged. No partial state.
- **Given**: Step 1 succeeds but step 2 fails
- **Expected**: Classification result is used. Memory store is NOT updated. Decay does NOT run. Classification result still provides compression benefit.
- **On failure**: Partial compression (classification applied, no memory extraction). Original history unchanged if step 1 fails.

#### [CP-16] Minimum context invariant

- **Given**: LLM classifies everything as prune (empty core output)
- **Input**: `apply_classification(history, all_prune_response)`
- **Expected**: Before returning, the function detects no user messages in core. Restores the last user message from the original history. Agent continues with at least: system prompt + compressed archive + last user message.
- **On failure**: Agent loses all conversation state (corruption).

#### [CP-17] Headroom enforcement

- **Given**: Compressed history token count exceeds `context_size × 0.75`
- **Input**: `enforce_headroom(compressed, context_size)`
- **Expected**: Oldest non-system messages are context-archived until the compressed history fits within 75% of the context window. Leaves 25% headroom for the next LLM response.

#### [CP-18] Manual compression (`/compress` command)

- **Given**: TUI sends `/compress` to Agent
- **Input**: `Agent::compress_now()` called
- **Expected**: Snapshots context. Runs full three-step pipeline (classify → extract → assemble). Replaces context with compressed result. Applies memory/skill ops. Updates telemetry.
- **On failure**: Context unchanged (pipeline returns original on any step failure).

#### [CP-19] Automatic compression in `chat_once()` (same prefix, append-only)

- **Given**: Gate triggers mid-conversation. KV cache for [system][conv 1..N] is live.
- **Input**: `chat_once()` detects gate → appends classify request to the prompt copy
- **Expected**: Classification runs as part of the SAME response as the agent's next turn. The model outputs: `[classification JSON]\n\n[natural response continues...]`. The agent strips the JSON block, applies it, and uses the natural response as the normal turn output.
- **On failure**: JSON parse fails → model response is used as-is (no compression this turn). History_ unchanged. Gate cooldown prevents retry for N turns.

---

### Integration with Agent Loop

The key integration change: the classify request is **not a separate compression-only call**. It's appended as part of the normal chat request:

```cpp
// In chat_once(), when gate fires:
if (gate_->should_compress(context_, cfg_)) {
    // Append classify instruction AFTER the user message
    prompt_copy.push_back(build_classify_request());
    // tools are still provided — model can use them AND classify
}

// Model response contains both:
//   [classification JSON]
//   ...
//   [normal assistant response]
//
// Pre-gate parsing separates them.
```

This means: the LLM classifies the conversation, outputs the JSON, AND continues its normal work — all in one generation. The classification tokens extend the KV cache from the conversation prefix. The response tokens extend it from the classification JSON. No separate requests, no prefill.

The extraction step (step 2) runs as a SEPARATE small request after the classification result is parsed. This is the only separate call — and it shares the same KV prefix as the classification request, so it extends incrementally.

---

### Cross-references

- **Depends on**: `llm-client/streaming.md` (LLM call), `memory/extraction.md` (memory/skill ops), `memory/memory-store.md`
- **Depended on by**: `agent-loop/core-loop.md` (compression gate in `chat_once()`)
- **Test coverage**: `tests/run_tests.cpp` — compression tests

### Known gaps

1. **No mock LLM in compression tests** — Pipeline tests verify sub-steps in isolation. Integration test with a real LLM only runs manually.

2. **Classification prompt quality** — The LLM's ability to distinguish active vs. completed investigations depends on prompt quality. Currently the prompt is generic — the redesigned prompt (with work-state heuristics) needs to be evaluated.

3. **Memory injection still changes the prompt prefix** — Memory/skill injection currently modifies the system prompt copy, which changes the prefix and triggers a prefill on every turn. This is a separate concern from compression: it affects regular turns, not compression steps. Documented in `memory/memory-store.md`.

4. **`CompressionBudget` not enforced** — Headroom enforcement (Invariant: 25% free after compression) is designed but not yet implemented. Low priority — the gate threshold already prevents overflow in most cases.
