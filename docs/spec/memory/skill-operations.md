## Spec: Learned Skill Operations

### Purpose

Manage the **learned skill** lifecycle within the `MemoryStore`: creation,
triggering, reconfirmation, conflict handling, and decay. Learned skills are the
agent's *automatic* procedural memory — recipes extracted by the LLM during
compression. They are one tier of the two-tier skills system; **authored
skills** (deliberate `SKILL.md` packages) are the other and live in
`docs/spec/skills/`. This spec covers only the learned tier inside the store.

### Ownership

- **Source files**: `lib/memory_store.cpp` (`JsonMemoryStore::upsert(Skill)`, `top_skills()`), `lib/compressor_apply.cpp` (`apply_skill_ops()`), `include/agent/experience.h` (`Skill`, `KnowledgeItem`)
- **Test files**: `tests/run_tests.cpp` — `skill_trigger` test

---

### What a Learned Skill Is

A learned skill is a **reusable procedure** that the agent follows when
triggered. It answers "how do I do X?" The `trigger_phrase` is the
call-to-action pattern — when the user says something matching it, the skill is
relevant.

**Example:**
```
Skill {
  name: "running-tests"
  content: "To run tests: call 'make test' in the workspace root.
            The run_tests binary has 150+ unit tests.
            Exceptions: integration tests may take 30s."
  trigger_phrase: "test"
  evidence_count: 7
  promoted: true
}
```

**How it gets used:**
1. User says "run the tests"
2. `top_skills()` finds this skill because "test" appears in the user message
3. The skill is injected into the prompt copy's learned-knowledge block
4. The model sees: *"When the user asks to run tests: execute 'make test'..."*
5. The model knows the exact command without having to discover it

**The dividing line from memories:**
| | Memory | Learned skill |
|---|---|---|
| **What** | Declarative fact (what/where/why) | Procedural recipe (how) |
| **Injected when** | Tags overlap with user message | Trigger phrase appears in user message |
| **Example** | "Build uses GNU Make" | "To build: run 'make' in the workspace root" |
| **Special fields** | (none) | `trigger_phrase` |

A skill without a trigger phrase is just a memory with extra fields. The
trigger phrase is what makes it actionable.

**Learned vs authored:** Learned skills are extracted automatically, live in the
project store, decay with disuse, and are private to the project. Authored
skills are written deliberately (`write_skill`/`/set skills create`, explicit
request only), live in `skills/` directories, and are portable. When both exist
under the same name, **authored wins** and the learned skill is suppressed from
injection. `export` graduates a learned skill into a global authored one — see
`skills/agent-skills.md` (precedence) and `skills/skill-catalog.md` ([SK-09]).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `KnowledgeOp{name, content, tags, action, trigger_phrase}` from compression |
| **Output** | Skill upserted/deprecated in store |
| **Error states** | Name conflict → skip silently. No trigger phrase → stored as regular skill (no trigger-based retrieval). |
| **Invariants** | See below. |

### Invariants

1. **Learned skills follow the same storage and decay rules as memories.** Same content-hash keying, same proportional decay, same conflict resolution. The only difference is the trigger phrase field and how retrieval works.

2. **Learned skills are retrieved by trigger phrase match.** A skill is returned from `top_skills()` if the user message contains its `trigger_phrase` (case-sensitive substring match). If no skill's trigger phrase matches, all promoted skills are returned as a fallback.

3. **No edits in place.** Same rule as memories: if the LLM returns a skill with the same name but different content, the op is skipped. Old knowledge is preserved. New knowledge creates a new entry.

4. **Reconfirmation strengthens.** Same rule as memories: same content hash on upsert → evidence increments. Skill becomes more durable.

5. **Deprecated skills are not deleted.** Evidence decremented by 1 (floor 0). Item stays in the store un-promoted, waiting for possible future reconfirmation.

6. **Skills without a trigger phrase are treated as memories with extra fields.** They can still be injected via the fallback path (all promoted skills) but won't trigger on user messages.

7. **Learned skills are project-scoped.** The store lives at `<workspace>/.amber/experience.json`; learned knowledge never leaks across projects. Cross-project reuse is the job of authored skills via `export`.

---

### Scenarios

#### [SO-01] Upsert learned skill from compression

- **Given**: LLM returns `"skills":[{"name":"deploy-cmd","content":"make deploy","trigger_phrase":"deploy","action":"upsert"}]`
- **Input**: `apply_skill_ops(response, store)`
- **Expected**: Skill created. `trigger_phrase = "deploy"`. `evidence_count = promote_threshold`. `promoted = true`.

#### [SO-02] Reconfirm existing skill

- **Given**: Existing skill with same name and same content hash
- **Input**: Same skill array again
- **Expected**: `evidence_count += 1`. Skill gains durability. Same rule as memory reconfirmation.

#### [SO-03] Skill conflict — name mismatch

- **Given**: Existing skill named "build-cmd" with content "make build"
- **Input**: Compression returns skill with same name but content "cmake --build"
- **Expected**: Conflict detected. Op silently skipped. Existing skill unchanged.
- **Rationale**: Same as memory conflicts — we don't edit in place.

#### [SO-04] Skill trigger in retrieval

- **Given**: Store has skills:
  - `"deploy-cmd"` with `trigger_phrase = "deploy"`
  - `"build-cmd"` with `trigger_phrase = "build"`
  - `"fix-cmd"` with `trigger_phrase = "fix"`
- **Input 1**: `store.top_skills(5, "how do I deploy the app")`
- **Expected 1**: Skill "deploy-cmd" returned (user message contains "deploy"). Other promoted skills also returned as fallback up to limit.
- **Input 2**: `store.top_skills(5, "how do I build the app")`
- **Expected 2**: Skill "build-cmd" returned. "deploy-cmd" NOT returned (no trigger match). Other promoted skills as fallback.
- **Input 3**: `store.top_skills(5, "hello world")`
- **Expected 3**: No trigger match. All promoted skills returned sorted by evidence.
- **Regression guard**: `skill_trigger` test.

#### [SO-05] Deprecate skill

- **Given**: Existing skill with matching content hash
- **Input**: `{"action":"deprecate","name":"old-skill"}`
- **Expected**: `evidence_count` decremented (floor 0). Not deleted.
- **Rationale**: Same as memory deprecate — keep the item, just remove it from promotion.

#### [SO-06] Skill without trigger phrase

- **Given**: LLM returns `"skills":[{"name":"note","content":"always check edge cases","action":"upsert"}]` — no trigger_phrase field
- **Input**: `apply_skill_ops(response, store)`
- **Expected**: Skill stored normally. `trigger_phrase` is empty. The skill will NOT match any user message on trigger phrase. It will only appear in the fallback path (all promoted skills).
- **Rationale**: Not every procedure needs a trigger. Some knowledge is always useful.

#### [SO-07] Skill grows with use

- **Given**: User frequently asks deployment-related questions. The skill "deploy-cmd" keeps getting confirmed in each compression cycle.
- **Input**: 5 compression cycles, each reconfirming the same skill
- **Expected**: `evidence_count` climbs with each reconfirmation. The skill becomes more durable against decay. Over time, it's always in the top-K because of high evidence despite any freshness decay.
- **Rationale**: Frequently needed skills become permanent. Infrequently needed skills decay away.

#### [SO-08] Authored skill shadows learned skill

- **Given**: Learned skill `run-tests` (score 0.8) in store; authored `run-tests` exists in `<workspace>/.amber/skills/`
- **Input**: Discovery block built; `top_skills()` called
- **Expected**: Authored `run-tests` listed in the catalog discovery; learned `run-tests` suppressed from injection (not deleted from the store).
- **Regression guard**: `docs/skills-tracker.md` item for precedence (spec `skills/agent-skills.md` [AS-03]).

#### [SO-09] Export graduates a learned skill

- **Given**: Learned skill `nightly-deploy` (score 0.8) in project store
- **Input**: `/set skills export nightly-deploy`
- **Expected**: Global authored `SKILL.md` written from store content; discovery re-run. Learned entry stays in store but is suppressed by precedence. Score untouched.
- **On failure**: Export modifies store or fails silently.

---

### Learned skills vs. Tools

Learned skills are NOT tools. They don't execute code. They're text injected
into the prompt that tells the model HOW to use existing tools effectively.

| | Tool | Learned skill |
|---|---|---|
| **What** | Executable function | Textual procedure |
| **How** | C++ code in `tools/` | Text injected into prompt copy |
| **When** | Model chooses to call it | Triggered by user message |
| **Persistence** | Recompilation | JSON store + compression extraction |
| **Example** | `read(path, offset)` | "To find a function: grep -rn 'func' src/" |

A skill can teach the model to use a tool more effectively. For example, a skill
"finding-functions" would tell the model "use the search tool with mode=grep and
pattern='func_name' to find function definitions." The model still calls the
search tool — the skill just provides the recipe. The same applies to authored
skills (which are activated via `read_skill`); see `skills/skill-catalog.md`.

---

### Cross-references

- **Depends on**: `memory/memory-store.md` (storage, decay), `memory/extraction.md` (extraction pipeline), `skills/agent-skills.md` (two-tier model, precedence), `skills/skill-catalog.md` (export, shadowing)
- **Depended on by**: `agent-loop/core-loop.md` (skill injection in prompt copy suffix)
- **Test coverage**: `tests/run_tests.cpp`: `skill_trigger` test

### Known gaps

1. **Trigger phrase is simple substring match** — No regex, no negation, no multi-phrase. If user asks "testing the waters" and trigger is "test", it matches even though they're not asking about tests. Acceptable for current scope — the model can filter in-context.

2. **No multi-trigger skills** — A skill can only have one trigger phrase. Some procedures could benefit from multiple trigger paths. Acceptable for current scope.

3. **No trigger phrase discovery** — The LLM must provide the trigger phrase explicitly when creating a skill. There's no automatic trigger detection from user message history. The LLM's judgment is the only input.

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Re-scoped to learned skills only (authored tier in `docs/spec/skills/`); dropped dead `steps`/`expected_outcome` fields; added precedence/shadowing + export + project-scope scenarios |
