## Spec: Skill Operations

### Purpose
Manage skill lifecycle within the `MemoryStore`: creation, triggering,
deprecation, and retrieval. Skills are a specialised `KnowledgeItem` with
a `trigger_phrase` that determines when they are injected into the context.

### Ownership
- **Source files**: `lib/memory_store.cpp` (`JsonMemoryStore::upsert(Skill)`, `top_skills()`), `lib/compressor_apply.cpp` (`apply_skill_ops()`), `include/agent/experience.h` (`Skill`)
- **Test files**: `tests/run_tests.cpp` — `skill_trigger` test

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `KnowledgeOp{name, content, tags, action, trigger_phrase}` from compression |
| **Output** | Skill upserted/deprecated in store |
| **Error states** | Name conflict → skip. |
| **Invariants** | See below. |

### Invariants

1. Skills have the same storage and decay rules as memories.
2. Skills only appear in retrieval if user message contains the `trigger_phrase`.
3. If no skill's trigger phrase matches, all promoted skills are returned.
4. Deprecated skills are not deleted — only evidence decremented.

---

### Scenarios

#### [SK-01] Upsert skill from compression

- **Given**: LLM returns `"skills":[{"name":"deploy-cmd","content":"make deploy","trigger_phrase":"deploy","action":"upsert"}]`
- **Input**: `apply_skill_ops(response, store)`
- **Expected**: Skill created with `trigger_phrase="deploy"`, `evidence_count=3`, `promoted=true`.

#### [SK-02] Skill trigger in retrieval

- **Given**: Store has skill `"deploy-cmd"` with `trigger_phrase="deploy"`
- **Input**: `store.top_skills(5, "how to deploy the app")`
- **Expected**: Skill returned (user message contains "deploy").
- **Input**: `store.top_skills(5, "how to build the app")`
- **Expected**: No trigger match. Falls back to all promoted skills.
- **Regression guard**: `skill_trigger` test.

#### [SK-03] Deprecate skill

- **Given**: Existing skill
- **Input**: `{"action":"deprecate","name":"old-skill"}`
- **Expected**: `evidence_count` decremented (floor 0). Not deleted.

#### [SK-04] Skill conflict — name mismatch

- **Given**: Existing skill named "build-cmd" with content "make build"
- **Input**: Compression returns skill with same name but content "cmake --build"
- **Expected**: Conflict detected. Op skipped. Existing skill unchanged.
- **On failure**: Skill overwritten with different content.

---

### Cross-references

- **Depends on**: `memory/memory-store.md`, `memory/extraction.md`
- **Depended on by**: `agent-loop/core-loop.md` (skill injection in system prompt suffix)
- **Test coverage**: `tests/run_tests.cpp`: `skill_trigger` test

### Known gaps

1. **Trigger phrase is simple substring match** — No regex, no negation, no multi-phrase.
2. **Force-promoted on creation** — Skills from compression are automatically `promoted=true` with evidence=3, bypassing the promotion threshold (5).
3. **No chaining** — Skills cannot reference or trigger other skills.
