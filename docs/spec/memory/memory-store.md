## Spec: Memory Store

### Purpose

Persist, score, and retrieve learned memories and skills across sessions. The store is the agent's long-term knowledge: facts about the world (memories) and procedures for doing things (skills). Knowledge grows with use, decays without it, and is never edited in place.

### Ownership

- **Source files**: `lib/memory_store.cpp` (`JsonMemoryStore`), `lib/memory_retriever.cpp` (`MemoryRetriever`), `include/agent/experience.h` (`Memory`, `Skill`, `MemoryStore`, `MemoryRetriever`, `ExperienceConfig`)
- **Test files**: `tests/run_tests.cpp` — memory tests, skill trigger tests, decay tests, retriever tests

---

### Architecture

```
KnowledgeItem                           Memory (is-a KnowledgeItem)
├── id: string                          ├── (no extra fields)
├── name: string                        │
├── content: string                     Skill (is-a KnowledgeItem)
├── tags: vector<string>                └── trigger_phrase: string
├── evidence_count: int
├── last_confirm_turn: int
├── score: double
└── promoted: bool
```

**Memory** (is-a KnowledgeItem) — declarative knowledge. Facts about the world, the project, the codebase. Answers "what?", "where?", "why?" Examples:
- "The build system uses GNU Make with a custom configure script"
- "Config files live under ~/.config/amber/"
- "Bug #42 was a null pointer dereference in parser.cpp"

**Learned skill** (is-a KnowledgeItem with trigger_phrase) — procedural knowledge. Recipes for accomplishing specific tasks. Answers "how?". Examples:
- "To run tests: call 'make test'. Trigger: 'test'"
- "To add a new tool: subclass Tool, implement execute(), register in register_default_tools(). Trigger: 'new tool'"
- "To find a function definition: grep -rn 'func_name' src/. Trigger: 'find function'"

**The dividing line:** Memory is a fact you reference. Skill is a procedure you follow. Both are knowledge, but one is passive (knowing) and one is active (doing). The trigger_phrase on skills is a call-to-action pattern — when the user says something matching it, the skill becomes relevant to inject.

**Learned vs authored:** Learned skills live in this store, are project-scoped, and decay. Authored skills live in `skills/` directories as `SKILL.md` packages (deliberate, portable) — see `docs/spec/skills/`. Same-name authored skills shadow learned ones; `export` graduates learned → authored.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `upsert(Memory)`, `upsert(Skill)`, `top_memories(k, user_msg)`, `top_skills(k, user_msg)`, `decay_all()`, `load(path)`, `save(path)` |
| **Output** | Persisted JSON file. Retrieval by relevance score. |
| **Error states** | File not found → empty store. JSON parse failure → empty store. Name conflict on upsert → op skipped silently. |
| **Store path** | Default `<workspace>/.amber/experience.json` (project-scoped); override `experience_store_path`. Legacy `~/.amber/memories.json` migration-seeded once ([MS-10]). |
| **Invariants** | See below. |

### Invariants

1. **No edits, ever.** When the LLM returns a memory or skill that conflicts with an existing one by name but has different content, the upsert is **silently skipped**. The old knowledge is preserved. New knowledge creates a new entry (with a different content hash). Conflict indicates the LLM is describing the same concept differently — we don't know which is correct, so we keep both and let evidence decide.

2. **Scoring determines injection priority.** Items are ranked by `score = evidence_count × 0.5 + relevance × 0.3 + freshness × 0.2`. Only the top-K are injected into the agent's context (`max_memories = 20`, `max_skills = 10`). The store can contain many more items — they sit on disk waiting for their relevance score to rise.

3. **Evidence is the durability primitive.** Each reconfirmation of the same content (same content hash on upsert) increments `evidence_count`. Items with high evidence survive more decay cycles. Items with low evidence disappear quickly.

4. **Promotion is controlled by threshold.** Items only appear in retrieval when `promoted = true`. An item is promoted when `evidence_count >= promote_threshold`. No more force-promotion — every item must earn its place through reconfirmations.

5. **Trigger phrases gate skill injection.** A skill is only injected when the user message contains its `trigger_phrase` (substring match). If no skill matches, all promoted skills are returned as fallback.

6. **Decay is proportional.** Each decay cycle reduces `evidence_count` by `max(1, evidence_count × decay_rate)`. Items erode proportionally: high-evidence items decay slowly (they're durable), low-evidence items decay quickly.

7. **Save is atomic.** Writes to `.tmp`, then `std::rename`.

---

### Scenarios

#### [MS-01] Upsert new memory — creation

- **Given**: Empty store
- **Input**: `store.upsert(Memory{name="proj-name", content="Project X uses GNU Make", tags=["build","make"], ...})`
- **Expected**: New entry created. Content hashed to `id`. `evidence_count = promote_threshold` (not 1 — the LLM confirmed it). `promoted = true`.
- **Regression guard**: `memory_store_upsert_and_retrieve` test.

#### [MS-02] Upsert existing memory — reconfirmation

- **Given**: Same content hash already exists
- **Input**: `store.upsert(Memory{name="proj-name", content="Project X uses GNU Make", ...})` — exact same content
- **Expected**: `evidence_count += 1` (capped at `threshold × 3`). `last_confirm_turn` updated. If not yet promoted and now meets threshold, `promoted = true`.
- **Rationale**: The same fact confirmed twice is more trustworthy.

#### [MS-03] Name conflict — same name, different content

- **Given**: Existing memory `name="build-cmd"` with `content="make build"`
- **Input**: `store.upsert(Memory{name="build-cmd", content="cmake --build", ...})` — same name, different content
- **Expected**: **Op skipped.** The existing entry is unchanged. A conflict is logged.
- **Rationale**: The LLM is describing the same concept differently. We don't know which is correct. Rather than overwrite or merge, we keep the original. If the new information is correct, the LLM will confirm it in a future compression cycle under a different name. If the original information is stale, it will decay.

#### [MS-04] Top memories — relevance scoring

- **Given**: Store with 50 memories
- **Input**: `store.top_memories(20, "tell me about the build system")`
- **Expected**: Returns 20 memories sorted by score descending. Only `promoted` items are considered. Memories with tags matching "build system" rank higher.
- **Regression guard**: `top_k_limits` test.

#### [MS-05] Top skills — trigger phrase match

- **Given**: Store has 15 skills, 5 with trigger_phrase containing "build"
- **Input**: `store.top_skills(10, "how do I build the project")`
- **Expected**: Skills with `trigger_phrase` matching "build" returned first. Remaining promoted skills as fallback. All `promoted = true`.
- **Regression guard**: `skill_trigger` test.

#### [MS-06] Proportional decay

- **Given**: Items with various `evidence_count`: 20, 10, 5, 2, 1
- **Input**: `store.decay_all()` with `decay_rate = 0.1`
- **Expected**: 
  - evidence=20 → 18 (10% = 2, floor 1)
  - evidence=10 → 9 (10% = 1)
  - evidence=5 → 4 (10% = 1, floor 1)
  - evidence=2 → 1 (10% = 1, floor 1)
  - evidence=1 → 0
- Items reaching 0 are set to `promoted = false`.
- **Rationale**: High-evidence items erode slowly (they're trustworthy). Low-evidence items disappear after a few cycles.
- **Regression guard**: `decay` test.

#### [MS-07] Save/load round-trip

- **Given**: Store with 15 items
- **Input**: `store.save(path)` then `store.load(path)` (fresh store)
- **Expected**: All 15 items preserved. Content hashes identical. Evidence counts, promotion status, scores intact.
- **On failure**: File corruption or missing items.

#### [MS-08] Store has more items than injection limit

- **Given**: Store has 200 memories, only 20 promoted
- **Input**: `store.top_memories(20, "build")`
- **Expected**: Only the 20 promoted items are scored and returned. The non-promoted 180 are ignored.
- **Rationale**: The limit is on injection, not storage. Items on disk don't cost tokens.

#### [MS-09] Authoring is not in the store

- **Given**: User runs `/set skills create foo`
- **Input**: `SkillCatalog` writes `foo/SKILL.md`; store untouched
- **Expected**: Authored skill lives in the `skills/` directory and the catalog; it does not appear in `MemoryStore` items or decay. (The two tiers are disjoint.)
- **On failure**: Authored skill mixed into learned store; decays or gets shadowed wrongly.

#### [MS-10] Project-scoped default + legacy migration

- **Given**: No `experience_store_path`; legacy `~/.amber/memories.json` exists; `<workspace>/.amber/experience.json` absent
- **Input**: First `Agent` construction in the workspace
- **Expected**: `load_experience_config` resolves the default store to `<workspace>/.amber/experience.json`; the store is seeded once from the legacy file; the legacy file is left untouched; subsequent runs load the project store.
- **On failure**: Learned skills disappear after upgrade, or the legacy global store keeps being read (cross-project leak).
- **Regression guard**: `docs/skills-tracker.md` item for store path + migration.

---

### Decay lifecycle

```
                    ┌─────────────┐
                    │ Compression │
                    │  produces   │
                    │ memory/skill│
                    └──────┬──────┘
                           │ upsert
                           ▼
                    ┌──────────────┐
                    │  promoted?   │── No ──► (waiting)
                    │ evidence >=  │
                    │  threshold   │
                    └──────┬──────┘
                           │ Yes
                           ▼
                    ┌──────────────┐
                    │  Injected    │
                    │  into prompt │◄── user message triggers
                    │  (if top-K)  │    relevance or trigger_phrase
                    └──────┬──────┘
                           │
                    (turns pass)
                           │
                    ┌──────▼──────┐
                    │  decay_all  │◄── next compression cycle
                    │  -10% evid. │
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐
                    │ evidence 0? │── Yes ──► depromote
                    │             │           (stay in store,
                    │             │            don't inject)
                    └─────────────┘
```

**Key principle:** No item is ever deleted. Depromoted items sit in the JSON file with evidence=0. If the LLM confirms them in a future compression, their evidence comes back (upsert resets evidence to threshold). If they're truly stale, they never get confirmed again and the JSON file simply grows. A future cleanup pass (manual or configurable) can prune items with evidence=0 older than N sessions.

---

### Cross-references

- **Depends on**: `memory/extraction.md`, `memory/skill-operations.md`, `skills/agent-skills.md` (two-tier model, budgets, precedence), `skills/skill-catalog.md` (export, [SK-09])
- **Depended on by**: `agent-loop/core-loop.md` (memory injection), `compression/compression-pipeline.md` (memory ops after compression)
- **Test coverage**: `tests/run_tests.cpp`: `memory_store_upsert_and_retrieve`, `top_k_limits`, `skill_trigger`, `decay`, `retriever_empty`, `retriever_with_memories`

### Known gaps

1. **`decay_rate` was ignored** — Now fixed with proportional decay (see [MS-06]). The config knob is alive.
2. **Force-promotion on creation** — Previously new items got evidence=3 promoted=true regardless of threshold. Now they use `promote_threshold`, so low-threshold items require fewer reconfirmations.
3. **In-memory only until save** — If process crashes between mutation and `save()`, all changes since last save are lost. Acceptable for current scope.
4. **Trigger phrase is simple substring match** — No regex, no negation, no multi-phrase. Acceptable for current scope.

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Dropped dead `steps`/`expected_outcome` from `Skill`; project-scoped store default + legacy migration ([MS-10]); two-tier cross-references |
