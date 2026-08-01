## Spec: Memory & Skill Extraction

### Purpose

Extract knowledge (memories and learned skills) from the LLM's compression
response and apply them to the `MemoryStore`. This is the only path for
**learning** — the old heuristic-based async extraction was removed. Every
extraction is driven by the LLM's judgment during the compression cycle.

Learned skills are one tier of the two-tier skills system; authored `SKILL.md`
skills are covered by `docs/spec/skills/`. This spec covers the learned tier's
extraction only.

### Ownership

- **Source files**: `lib/agent.cpp` (`Agent::apply_compression_result()` — extracted from anonymous namespace), `lib/compressor_apply.cpp` (`apply_memory_ops()`, `apply_skill_ops()`), `lib/memory_retriever.cpp` (`MemoryRetriever::build_system_prompt_suffix()`), `lib/memory_store.cpp` (`JsonMemoryStore`)
- **Test files**: `tests/run_tests.cpp` — extraction tests

---

### Architecture

```
Compression cycle
       │
       ▼
CompressionResponse
  ├── segments[]    (core/context/prune classification)
  ├── memory_ops[]  (KnowledgeOp — upsert/deprecate)
  └── skill_ops[]   (KnowledgeOp — upsert/deprecate with trigger_phrase)       │
       ▼
Agent::apply_compression_result(cr)
  ├── 1. apply_memory_ops(store, cr.memory_ops, path)
  │     ├── upsert new memory (evidence = promote_threshold)
  │     ├── upsert existing (same content hash → evidence++)
  │     └── conflict (same name, different content → SKIP)
  │
  ├── 2. apply_skill_ops(store, cr.skill_ops, path)
  │     ├── upsert new skill (evidence = promote_threshold)
  │     ├── upsert existing (same content hash → evidence++)
  │     └── conflict (same name, different content → SKIP)
  │
  ├── 3. store.decay_all()
  │     └── proportional: evidence -= max(1, evidence × decay_rate)
  │
  └── 4. store.save(path)
        └── atomic write to .tmp + rename
```

**Key principle:** Extraction is a side effect of compression. It never runs independently. Every compression cycle produces exactly one extraction cycle. If no memories or skills are identified, the store is unchanged (decay still runs).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `CompressionResponse` with `memory_ops` and `skill_ops` arrays + `MemoryStore` + `ExperienceConfig` |
| **Output** | MemoryStore upserted/deprecated items + decay + atomic save |
| **Error states** | Name conflict → op silently skipped. Store path empty → no save. |
| **Invariants** | See below. |

### Invariants

1. **Extraction only happens during compression.** There is no standalone extraction path. Every extraction is a side effect of a compression cycle (manual or automatic).

2. **Conflict = skip, not overwrite.** If the LLM returns a knowledge op with the same name but different content as an existing item, the op is skipped. The old item is preserved unmodified. This prevents edit wars between compression cycles where the LLM disagrees with itself.

3. **Reconfirmation = strengthen.** If the LLM returns a knowledge op with the same name AND same content (matching content hash) as an existing item, the item's `evidence_count` increments. The fact is confirmed again — it's more trustworthy.

4. **New items start at promote_threshold, not 1.** When the LLM creates a new memory or skill, its initial `evidence_count` is set to the store's `promote_threshold` and it is immediately `promoted = true`. The LLM's single confirmation is treated as sufficient for initial promotion. Further reconfirmations increase evidence; failure to reconfirm causes decay.

5. **Decay runs every extraction cycle, even when no ops are returned.** `decay_all()` is unconditional. Absence of reconfirmation IS the signal that knowledge is not being used.

6. **Save is atomic.** Uses `.tmp` + `std::rename` pattern. Incomplete writes don't corrupt the store.

---

### Scenarios

#### [ME-01] Upsert new memory from compression

- **Given**: LLM returns `"memories":[{"name":"proj-name","content":"Project X uses GNU Make","tags":["build","make"],"action":"upsert"}]`
- **Input**: `apply_memory_ops(response, store)`
- **Expected**: New memory created with `evidence_count = promote_threshold`, `promoted = true`. Saved to store.
- **Regression guard**: `integration_apply_and_retrieve` test.

#### [ME-02] Reconfirm existing memory (same content)

- **Given**: Existing memory with name "proj-name" and content "Project X uses GNU Make" (matching content hash)
- **Input**: Same `memories` array from ME-01 again
- **Expected**: Existing memory found by content hash. `evidence_count += 1`. `last_confirm_turn` updated. No duplicate created.
- **Rationale**: The LLM verified the same fact in a subsequent compression. The fact is more trustworthy.

#### [ME-03] Name conflict — different content, same name

- **Given**: Existing memory with name "proj-name" and content "Project X uses GNU Make"
- **Input**: `"memories":[{"name":"proj-name","content":"Project X uses CMake now","action":"upsert"}]`
- **Expected**: **Conflict detected.** Op skipped. Existing memory unchanged. No edits in place.
- **Rationale**: The LLM is describing the same concept differently. Neither version is more trustworthy. The original persists. If the new information is correct, the LLM should create it under a different name. The original decays naturally.

#### [ME-04] Deprecate existing memory

- **Given**: Existing memory with matching content hash
- **Input**: `{"action":"deprecate","name":"old-info"}`
- **Expected**: `evidence_count` decremented by 1 (floor 0). Item is NOT deleted — it stays in the store un-promoted.
- **Rationale**: Deprecate is a single-cycle signal. It reduces evidence but doesn't remove the item. If the item is reconfirmed in a future compression (ME-02), its evidence comes back.

#### [ME-05] Skill upsert with trigger phrase

- **Given**: LLM returns `"skills":[{"name":"deploy-cmd","content":"use make deploy","trigger_phrase":"deploy","action":"upsert"}]`
- **Input**: `apply_skill_ops(response, store)`
- **Expected**: Learned skill created. `trigger_phrase = "deploy"`. `evidence_count = promote_threshold`. `promoted = true`. On next retrieval, user message containing "deploy" triggers this skill.
- **Note**: The old `steps`/`expected_outcome` skill fields were dead (serialized but never populated by the extraction prompt) and are dropped — a learned skill is a single `content` entry plus `trigger_phrase`, matching `KnowledgeItem`.

#### [ME-06] Skill reconfirmation (same content)

- **Given**: Existing skill with same content hash
- **Input**: Same skill array again
- **Expected**: `evidence_count += 1`. Skill gains durability. Same pattern as memory reconfirmation.

#### [ME-07] Skill conflict — name mismatch

- **Given**: Existing skill named "deploy-cmd" with content "make build"
- **Input**: Compression returns skill with same name but content "cmake --build"
- **Expected**: Conflict detected. Op skipped. Existing skill unchanged. Same rule as memories.

#### [ME-08] Decay runs even when no ops returned

- **Given**: Store with 5 items (all evidence > 0). Compression returns empty `memory_ops` and `skill_ops`.
- **Input**: `apply_compression_result(empty_response)`
- **Expected**: `decay_all()` runs. All items lose proportional evidence. Items reaching 0 are depromoted.
- **Rationale**: Absence of reconfirmation across a compression cycle means the knowledge isn't being used. It should decay.

#### [ME-09] Memory retrieval — system prompt suffix

- **Given**: MemoryStore has 5 memories (3 promoted), 2 skills (1 with matching trigger phrase). User says "tell me about the build system".
- **Input**: `retriever.build_system_prompt_suffix("tell me about the build system", 500)`
- **Expected**: Returns formatted string with `"=== Learned Knowledge ==="` header. Top 20 memories by relevance score. Top 10 skills with matching trigger phrase. Truncated to ~2000 chars (500 tokens × 4).
- **On failure**: Knowledge block missing from prompt. Empty store → empty suffix → skipped.

#### [ME-10] Store version tracking — skip injection when unchanged

- **Given**: MemoryStore has a version counter. Compression ran on turn N, extracted 2 memories. Turn N+1 runs with same store (no new memories).
- **Input**: `chat_once()` calls `retriever.build_system_prompt_suffix()`
- **Expected**: Store version matches the cached version on Agent. Injection skipped. System prompt unchanged from previous turn.
- **Rationale**: Avoids modifying the system prompt copy when nothing changed. Reduces forward-pass cost for the injected portion.

---

### Injection into context

Memories and learned skills are injected into the **prompt copy** before every LLM call:

```
[system] "You are an AI assistant..."
[injected knowledge] "=== Learned Knowledge ===
  Memory \"build-system\": Project X uses GNU Make  (evidence: 8)
  Skill \"run-tests\": To run tests, call 'make test'  [trigger: test]
=== End Learned Knowledge ==="
```

The authored-skill **discovery metadata** block is injected as a *separate slot*
immediately after this learned-knowledge block (fixed order preserves the
KV-cache prefix), and activated skill bodies append later slots on demand. See
`skills/agent-skills.md` (injection-slot rules, [AS-01]).

**This is done on a COPY of the prompt**, not the live context. The live
context's system message is never modified. This ensures the KV cache within
each request is maximally reused — the system prompt prefix is the same until
the injection block.

**Optimization:** The injection is only performed when the store's version has
changed since the last call. If no new memories or skills were added/removed
between turns, the injection block is identical and can be skipped (the
per-request KV cache still processes it, but the HTTP overhead is reduced).

**Store location:** The learned store is **project-scoped** by default at
`<workspace>/.amber/experience.json` (override via `experience_store_path`). The
legacy global `~/.amber/memories.json` is migration-seeded once on first use —
see `memory/memory-store.md` ([MS-10]) and `skills/agent-skills.md` ([AS-09]).

---

### Cross-references

- **Depends on**: `compression/compression-pipeline.md` (provides `CompressionResponse`), `memory/memory-store.md` (storage and retrieval), `skills/agent-skills.md` (two-tier model, injection-slot rules)
- **Depended on by**: `agent-loop/core-loop.md` (memory injection in chat_once), `memory/skill-operations.md` (learned skill lifecycle)
- **Test coverage**: `tests/run_tests.cpp`: `integration_apply_and_retrieve`, `agent_extract_memories_from_tool_results`

### Known gaps

1. **Name-only conflict detection** — Conflict is detected by name only. If the LLM uses two different names for the same fact, both are created. No content deduplication. Acceptable for current scope — duplicates will compete on evidence and one will outlive the other.

2. **No undo/rollback for extraction** — Once `apply_memory_ops` modifies the store, there's no rollback if a subsequent step fails. The store is persisted independently. Acceptable for current scope.

3. **Store version tracking not yet implemented** — The optimization to skip injection on unchanged store is designed but not coded. Currently every turn pays the injection cost. Low priority — the cost is ~500 tokens of compute per turn.

4. **Decay rate was hardcoded to -1** — Now fixed to use the configured `decay_rate` with proportional decay (see `memory-store.md` [MS-06]).

5. **Learned skills are the automatic tier only** — Extraction never produces authored `SKILL.md` packages. Authoring is explicit-request only (`write_skill`/`/set skills create`); see `skills/skill-catalog.md` [SK-05].

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Dropped dead `steps`/`expected_outcome` skill fields; injection order with authored-skill discovery slot; project-scoped store + legacy migration note; two-tier cross-references |
