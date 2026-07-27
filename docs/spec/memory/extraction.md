## Spec: Memory Extraction

### Purpose
Extract knowledge (memories and skills) from the LLM's compression response
and apply them to the `MemoryStore`. This is the only path for learning —
the old heuristic-based async extraction was removed.

### Ownership
- **Source files**: `lib/agent.cpp` (`apply_compression_memops()` — lines 161–192), `lib/compressor_apply.cpp` (`apply_memory_ops()`, `apply_skill_ops()`), `lib/memory_retriever.cpp` (`MemoryRetriever::build_system_prompt_suffix()`)
- **Test files**: `tests/run_tests.cpp` — experience extraction tests (lines 1788–1975)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `CompressionResponse` with `memory_ops` and `skill_ops` arrays + `MemoryStore` |
| **Output** | MemoryStore upserted/deprecated items + decay + save |
| **Error states** | Name conflict → op silently skipped. Store path empty → no save. |
| **Invariants** | See below. |

### Invariants

1. Memory extraction only happens during manual or automatic compression.
2. `upsert` actions create new items with `evidence_count = 3`, `promoted = true`.
3. `deprecate` actions decrement `evidence_count` (floor 0).
4. Name conflicts (same name + different content) cause the op to be skipped.
5. After all ops, `decay_all()` runs, then `save()` persists.

---

### Scenarios

#### [ME-01] Upsert new memory

- **Given**: LLM returns `"memories":[{"name":"proj-name","content":"Project X","action":"upsert"}]`
- **Input**: `apply_memory_ops(response, store)`
- **Expected**: New memory created: `evidence_count=3`, `promoted=true`. Saved to store.
- **Regression guard**: `integration_apply_and_retrieve` test.

#### [ME-02] Deprecate existing memory

- **Given**: Existing memory with matching content hash
- **Input**: `{"action":"deprecate","name":"old-info"}`
- **Expected**: `evidence_count` decremented (floor 0). Content not deleted.
- **On failure**: Evidence goes negative.

#### [ME-03] Name conflict — skip

- **Given**: Existing memory with name "proj-name" but different content
- **Input**: `{"name":"proj-name","content":"Different content","action":"upsert"}`
- **Expected**: Conflict detected. Op silently skipped. No change.
- **On failure**: Overwritten with different content.

#### [ME-04] Skill upsert with trigger phrase

- **Given**: LLM returns skill with trigger_phrase
- **Input**: `{"name":"deploy-cmd","content":"use make deploy","trigger_phrase":"deploy"}`
- **Expected**: Skill created. `trigger_phrase = "deploy"`. On next retrieval, user message containing "deploy" triggers this skill.

#### [ME-05] Memory retrieval — system prompt suffix

- **Given**: MemoryStore has 5 memories, user asks "tell me about project X"
- **Input**: `retriever.build_system_prompt_suffix(user_msg, 500)`
- **Expected**: Returns formatted string with `"=== Learned Knowledge ==="` header, top memories scored by relevance, top triggered skills. Truncated to ~2000 chars (500 tokens × 4).
- **On failure**: Knowledge block missing from prompt.

#### [ME-06] Decay after extraction

- **Given**: All items have `evidence_count >= 1`
- **Input**: `store.decay_all()`
- **Expected**: Each item's evidence_count decremented by 1. Items reaching 0 are set to `promoted = false`.
- **Known bug**: `decay_rate` from config is declared but ignored — always decrements by exactly 1.

---

### Cross-references

- **Depends on**: `compression/compression-pipeline.md`, `memory/memory-store.md`
- **Depended on by**: `agent-loop/core-loop.md` (memory injection in chat_once)
- **Test coverage**: `tests/run_tests.cpp`: `integration_apply_and_retrieve` (1878–1949), `agent_extract_memories_from_tool_results` (1955–1975)

### Known gaps

1. **`decay_rate` unused** — Config permits fractional decay (0.1), but implementation always decrements by 1.
2. **New memories force-promoted** — evidence=3 bypasses the promotion threshold. Every compression-cycle discovery is immediately promoted.
3. **Name-only conflict detection** — If the LLM reuses a name with different content, the op is silently skipped. No retry or merge strategy.
