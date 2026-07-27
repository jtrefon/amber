## Spec: Memory Store

### Purpose
Persist, query, and decay learned memories and skills. Uses a JSON-backed
`JsonMemoryStore` that stores items in an unordered map keyed by content hash.
Supports relevance-scored retrieval, keyword (tag) matching, and skill trigger
phrase detection.

### Ownership
- **Source files**: `lib/memory_store.cpp` (`JsonMemoryStore`, 322 lines), `lib/memory_retriever.cpp` (`MemoryRetriever`), `include/agent/experience.h`
- **Test files**: `tests/run_tests.cpp` — 6 memory tests (lines 1788–1872)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `upsert(Memory)`, `upsert(Skill)`, `top_memories(k, user_msg)`, `top_skills(k, user_msg)`, `decay_all()`, `load(path)`, `save(path)` |
| **Output** | Persisted JSON file. Retrieval by relevance score. |
| **Error states** | File not found → empty store. JSON parse failure → empty store. |
| **Invariants** | See below. |

### Invariants

1. All items are keyed by content hash (`id`).
2. Only `promoted` items appear in query results.
3. Memory scoring: evidence (×0.5) + relevance (×0.3) + freshness (×0.2).
4. Skill triggering: user message must contain the skill's `trigger_phrase`.
5. Save is atomic: writes to `.tmp`, then `std::rename`.
6. `decay_all()` decrements evidence by exactly 1 (ignores `decay_rate` config).

---

### Scenarios

#### [MS-01] Upsert new memory

- **Given**: Empty store
- **Input**: `store.upsert(Memory{"", "proj-name", "Project X", {"project"}, 0, 0})`
- **Expected**: New entry: `evidence_count = 0`, `promoted = false` (not promoted until threshold).
- **Regression guard**: `memory_store_upsert_and_retrieve` test.

#### [MS-02] Upsert existing memory (bump evidence)

- **Given**: Same memory already exists
- **Input**: `store.upsert(Memory{..., "proj-name", "Project X", ...})` (same content hash)
- **Expected**: `evidence_count += 1`. If evidence >= threshold, `promoted = true`.
- **On failure**: Duplicate entry created.

#### [MS-03] Top memories — scored

- **Given**: Store with 3 memories, one very relevant
- **Input**: `store.top_memories(2, "tell me about the build system")`
- **Expected**: Returns 2 memories sorted by score descending. Most relevant (matching tags) on top. All `promoted = true`.
- **Regression guard**: `top_k_limits` test.

#### [MS-04] Top skills — trigger phrase match

- **Given**: Store with skill `trigger_phrase = "deploy"`
- **Input**: `store.top_skills(5, "how do I deploy")`
- **Expected**: Skill returned because user message contains "deploy". If no trigger match, returns all promoted skills.
- **Regression guard**: `skill_trigger` test.

#### [MS-05] Decay all — evidence decreases

- **Given**: Items with various evidence counts
- **Input**: `store.decay_all()`
- **Expected**: Each item's evidence decremented by 1. Items at 0 set to `promoted = false`.
- **Regression guard**: `decay` test.

#### [MS-06] Save/load round-trip

- **Given**: Store with items
- **Input**: `store.save(path)` then `store.load(path)`
- **Expected**: All items preserved. Content hashes identical.
- **On failure**: File corruption or missing items.

---

### Cross-references

- **Depends on**: `memory/extraction.md`, `memory/skill-operations.md`
- **Depended on by**: `agent-loop/core-loop.md` (memory injection), `compression/compression-pipeline.md` (memory ops after compression)
- **Test coverage**: `tests/run_tests.cpp`: `memory_store_upsert_and_retrieve`, `top_k_limits`, `skill_trigger`, `decay`, `retriever_empty`, `retriever_with_memories`

### Known gaps

1. **`decay_rate` config ignored** — Always decrements by 1 regardless of configured rate.
2. **In-memory only until save** — If process crashes between mutation and `save()`, all changes since last save are lost.
3. **No TTL / expiry** — Items survive indefinitely unless explicitly deprecated or decayed to 0.
