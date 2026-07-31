# amber — Skills System Implementation Tracker

- **Status:** 🟢 Complete — SK-IMP-001..007 implemented on `feat/skills-system`, all gates green
- **Reference:** `docs/spec/skills/agent-skills.md`, `docs/spec/skills/skill-files.md`, `docs/spec/skills/skill-catalog.md`
- **Issues register:** `docs/issues.md`

---

## How to Use This Tracker

1. Every task follows the **Red → Proposal → Sign-off → Green → PR** workflow
   (see AGENTS.md). On a fresh branch named `<type>/<short-description>`:
   - **Red**: Write a failing test first (scenario IDs below map to the spec),
     commit it so CI shows the failure.
   - **Proposal**: Link the task below in the PR description.
   - **Sign-off**: Reviewer approves the proposed architecture.
   - **Green**: Implement; make the test pass; refactor to zero debt
     (classes ≤200 lines, methods ≤10 lines, SOLID, hexagonal boundaries).
   - **PR**: Open/update. All checks must pass.
2. Each task is **self-contained** and ordered by dependency (prerequisites first).
3. **Verification** must pass before marking a task `[done]`:
   `make clean && make && make test && make lint && make analyze`.
   If headers change, `make clean && make` regenerates the `.d` files.
4. No comments that restate code; first line of new files is functional
   (no SPDX/copyright boilerplate). Run `clang-format -i` on touched files.
5. Spec scenarios are the acceptance contract — the test names below reference
   the scenario IDs (e.g. `[SF-01]`), and the tracker row links them.

## Legend

```
[ ]     — Not started, ready for assignment
[~]     — Assigned and actively being worked
[x]     — Code merged, all checks pass, no known regressions
[!]     — Blocked on another task or external dependency
```

---

## Task 1: Drop dead Skill fields (SK-IMP-001)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-001` |
| **Severity** | 🟡 Medium |
| **Depends on** | None |
| **Blocks** | Nothing (reduces touch conflicts with SK-IMP-002) |
| **Estimated effort** | 1-2 hours |
| **Files touched** | `include/agent/experience.h`, `lib/memory_store.cpp`, `lib/compressor_apply.cpp`, compression prompt in `prompts/` |
| **Spec refs** | `memory/extraction.md` [ME-05] note, `memory/memory-store.md` struct diagram, `memory/skill-operations.md` |

### Problem

`Skill` (`include/agent/experience.h:31-35`) carries `steps` and
`expected_outcome` fields that are serialized but **never populated** — the
extraction prompt and `apply_skill_ops` never set them. Dead schema debt.

### Target Architecture

- `Skill` is `KnowledgeItem` + `trigger_phrase` only.
- Serialization no longer emits `steps`/`expected_outcome`; loading a legacy
  file that contains them ignores the keys (forward-compatible read).
- `apply_skill_ops` and the extraction prompt unchanged in behaviour.

### Refactor Rules

- Remove the fields from the struct. Do NOT keep them read-for-compat in code
  (the parser ignores unknown keys).
- Update the compression/experience prompt if it references step-based skills.

### Verification

- [x] Red test `skill_no_dead_fields`: serialize a `Skill`, assert no
      `steps`/`expected_outcome` keys in JSON; loading a legacy JSON that has
      them round-trips without them.
- [x] `make clean && make && make test && make lint && make analyze` clean
- [x] `grep -rn 'expected_outcome\|\.steps' include/ lib/ tests/` returns nothing
      for the skill struct (distinguish from `Memory::steps` if any).

---

## Task 2: Project-scoped experience store + legacy migration (SK-IMP-002)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-002` |
| **Severity** | 🟠 High |
| **Depends on** | None |
| **Blocks** | Shadowing/precedence behaviour in SK-IMP-004 |
| **Estimated effort** | 3-4 hours |
| **Files touched** | `lib/config.cpp`, `include/agent/experience.h`, `lib/memory_store.cpp`, `lib/agent.cpp` (store construction), `tests/run_tests.cpp` |
| **Spec refs** | `memory/memory-store.md` [MS-10], `skills/agent-skills.md` [AS-09], `config/file-config.md` [FC-09]/[FC-10] |

### Problem

Learned skills/memories persist at `~/.amber/memories.json` (global), leaking
project-specific procedures across unrelated workspaces. `experience_store_path`
defaults are not project-scoped.

### Target Architecture

- Default store path: `<workspace>/.amber/experience.json`
  (`Workspace::local_dir()` + `experience.json`). `experience_store_path` in
  config still overrides absolutely.
- On first `Agent` construction in a workspace: if the project store is absent
  and legacy `~/.amber/memories.json` exists, **seed once** from the legacy file
  into the project store. Legacy file untouched. Subsequent runs use the project
  store (a marker/version records that the seed ran).
- `load_experience_config` resolves the path.

### Refactor Rules

- Seeding is one-time and idempotent: never copy on a second run, never merge on
  every start.
- Do not write to the legacy global file.
- Keep `experience_store_path` override working exactly as before (absolute path
  wins).

### Verification

- [x] Red test `experience_store_project_default`: config empty →
      `load_experience_config` resolves to `<workspace>/.amber/experience.json`
- [x] Red test `experience_store_legacy_seed_once`: legacy file + absent project
      store → seeded; legacy untouched; second construction does not re-seed
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 3: SKILL.md parser + directory scanner (SK-IMP-003)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-003` |
| **Severity** | 🟠 High |
| **Depends on** | None |
| **Blocks** | SK-IMP-004 (catalog), SK-IMP-005 (tools), SK-IMP-007 (trust) |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `include/agent/skill_file.h` (new), `lib/skill_file.cpp` (new), `tests/skill_file_test.cpp` (new, add to `UNITTEST_OBJ` in `Makefile.in`), `include/agent/config.h` (`skills_interop`) |
| **Spec refs** | `skills/skill-files.md` [SF-01]–[SF-09], `skills/agent-skills.md` [AS-04]/[AS-08] |

### Problem

No authored-skill layer exists. Need a dependency-free tolerant frontmatter
parser and a directory scanner with scope precedence and the interop gate.

### Target Architecture

- `SkillMeta { name, description, license, compatibility, metadata(json), body }`
  and `SkillFile { name, path, scope, meta, enabled }` value types.
- `parse_skill_meta(contents) -> optional<SkillMeta>`: first `---`-delimited
  block; single-line + `>`-folded values; unknown keys ignored; malformed →
  `nullopt` (undiscoverable, never crashes).
- `scan_skill_dir(root, scope) -> vector<SkillFile>`: one level deep, hidden
  entries excluded, missing root → empty (not an error), unreadable file →
  warned + excluded, kebab-case name gate.
- `scan_skills(project, global, interop_enabled)` returns the **deduped, ordered**
  union (project → global → interop), first occurrence of a name wins.
- Config key `skills_interop` (default `false`).

### Refactor Rules

- No YAML dependency — subset parser only. Do not add a YAML library.
- Parser is a pure function; scanner holds no state.
- Symlink/escape checks reuse `Workspace::confine` semantics.

### Verification

- [x] Tests `[SF-01]`–`[SF-09]` each get a small `TEST` block in
      `tests/skill_file_test.cpp` (minimal valid, folded description, malformed
      excluded, dir/name mismatch, project shadows global, interop gate,
      unknown keys ignored, non-kebab excluded, missing root empty)
- [x] `make clean && make && make test && make lint && make analyze` clean
- [x] No `#include "tui/"` or `#include "src/"` from `lib/skill_file.cpp`

---

## Task 4: SkillCatalog + overrides + discovery injection (SK-IMP-004)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-004` |
| **Severity** | 🟠 High |
| **Depends on** | SK-IMP-003 (scanner), SK-IMP-002 (learned shadowing) |
| **Blocks** | SK-IMP-005 (tools), SK-IMP-006 (commands) |
| **Estimated effort** | 5-8 hours |
| **Files touched** | `include/agent/skill_catalog.h` (new), `lib/skill_catalog.cpp` (new), `include/agent/config.h` (budgets), `lib/agent.cpp` (injection slot), `tests/skill_catalog_test.cpp` (new) |
| **Spec refs** | `skills/skill-catalog.md` [SK-01]/[SK-06]/[SK-07]/[SK-08]/[SK-13], `skills/agent-skills.md` [AS-01]–[AS-07]/[AS-10]/[AS-11] |

### Problem

No runtime union view, no persisted curation, no discovery-block injection.

### Target Architecture

- `SkillCatalog` port: `discover()`, `lookup(name)`, `read_body(name)`
  (budget-capped, cached), `apply_overrides()`, `discovery_block()`.
- `SkillOverrides`: JSON at `<workspace>/.amber/skills.json` + global
  `~/.config/amber/skills.json`, merged project-wins. States
  `enable`/`disable`/`block`; override beats precedence; `disable`/`block` are
  sticky across re-scans.
- Precedence resolution: `override > project > global > interop > learned`;
  authored shadows learned (suppressed, not deleted).
- Budgets: `skills_max_discovery` (default 20, drop by scan order),
  `skills_body_budget_tokens` (default 5000, reject not truncate).
- Injection: discovery block as a **separate message slot** on the prompt copy
  after system + learned-knowledge (stable prefix). Mid-session `refresh`
  rebuilds the block (documented KV-cache cost).

### Refactor Rules

- Injection never mutates the live context stack (`Context` hash chain intact) —
  only the per-request prompt copy in `Agent::chat_once`, same path as
  `MemoryRetriever`.
- Catalog holds no LLM state; discovered once at session start, rebuilt only on
  explicit `refresh`/command.
- `read_body` caches per-name; cache invalidated on `discover()`.

### Verification

- [x] Tests for [SK-01] (union discovery), [AS-05]/[SK-06] (disable sticky),
      [SK-07] (enable forces shadowed), [SK-08] (block provenance),
      [AS-06] (discovery budget), [AS-07] (oversized body rejected)
- [x] Tests for authored-shadows-learned [AS-03] and project-shadows-global
      [AS-04] end-to-end through the catalog
- [x] Test [AS-10]: after refresh, discovery block changed; next prompt copy
      contains the new block
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 5: Skill tools — read_skill, write_skill, list_skills (SK-IMP-005)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-005` |
| **Severity** | 🟠 High |
| **Depends on** | SK-IMP-004 (catalog) |
| **Blocks** | SK-IMP-006 (commands share authoring path) |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `tools/skill_tools.cpp` (new), `lib/agent.cpp` (activation slot append), `prompts/tools.md`, `tests/skill_catalog_test.cpp` (tool tests) |
| **Spec refs** | `skills/skill-catalog.md` [SK-02]/[SK-03]/[SK-04]/[SK-05]/[SK-14]/[SK-15], `skills/agent-skills.md` [AS-02] |

### Problem

The model has no way to activate a skill, list them, or author one.

### Target Architecture

- `ReadSkillTool` (read-only, never approval): loads + caches body via catalog,
  budget-capped; unknown name → `ToolResult{ok=false}`. Harness appends the body
  as a new message slot on the *next* prompt copy.
- `ListSkillsTool` (read-only): returns the discovery block, optional `name`
  substring + `origin` filters.
- `WriteSkillTool` (write, always approval): validates kebab-case name, writes
  `SKILL.md` to project or global scope, triggers `discover()`. **Explicit
  request only** — the tools prompt instructs the model never to author
  unsolicited; the approval gate is the backstop ([SK-05]).
- Register in `register_default_tools`.

### Refactor Rules

- Implement the `Tool` interface (`include/agent/tool.h`) — no new execution
  surface, no running of `scripts/`.
- `write_skill` shares one authoring helper with `/set skills create`
  (SK-IMP-006) — DRY.

### Verification

- [x] Tests: [SK-02] activation + cache, [SK-03] unknown name error,
      [SK-14] oversized body rejected, [SK-15] filtered list,
      [SK-04]/[SK-05] write + approval/denial
- [x] `read_skill` never requests approval; `write_skill` always does
      (assert via `requires_approval` in tests)
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 6: /set skills + /get skills command subtree (SK-IMP-006)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-006` |
| **Severity** | 🟠 High |
| **Depends on** | SK-IMP-004 (catalog/overrides), SK-IMP-005 (authoring helper) |
| **Blocks** | Nothing |
| **Estimated effort** | 5-8 hours |
| **Files touched** | `tui/tui_input.cpp`, `tui/tui_session.cpp`, `include/agent/skill_catalog.h`, `tests/tui_tests.cpp` |
| **Spec refs** | `tui/input-system/nested-commands.md` [NC-22]–[NC-25], `skills/skill-catalog.md` [SK-09]–[SK-13]/[SK-16], `skills/agent-skills.md` [AS-09]/[AS-11] |

### Problem

No user-facing curation surface. Skills cannot be created, deleted, exported,
re-scanned, inspected, or suppressed from the TUI.

### Target Architecture

Extend the `set`/`get` command nodes per the target tree in
`nested-commands.md`:

```
set skills
  interop <on|off>       refresh          show [name]
  create <name> [--global]   delete <name> [--global]
  export <name>              enable <name> | disable <name> | block <name>
get skills [name]
```

- `create`/`delete` default to project scope; `--global` selects global.
- `export` graduates a learned skill → global authored `SKILL.md` (one-way;
  store score untouched).
- `refresh` re-runs `discover()`; no automatic background scans.
- `show`/`get skills` render the scope table
  (`project/global/interop/learned · authored/learned · name · state`, plus
  `suppressed` for shadowed learned skills).
- `enable`/`disable`/`block` persist to the overrides file immediately.

### Refactor Rules

- Use the `CommandNode` tree model (fixed subcommands + ArgSpec + FlagSpec);
  no new manual string parsing.
- Shared authoring logic comes from SK-IMP-005; export reuses the catalog's
  learned lookup.

### Verification

- [x] Command logic covered by `skill_commands_*` tests in
      `tests/skill_catalog_test.cpp` ([NC-22] show, [NC-23] export incl.
      unknown learned skill error, [NC-24] create with `--global`, [NC-25]
      disable persists across refresh). Note: Tui itself needs ncurses and is
      not unit-testable; the TUI handlers are thin glue over the tested
      `lib/skill_commands.cpp` functions.
- [x] Test [SK-16]: interop off → skills absent; enabling + refresh → present
      (`skill_catalog_interop_gate`)
- [x] Test [SK-12]: `create --global` writes under `~/.config/amber/skills/`
      (`skill_commands_create_global`)
- [~] Manual: `/set skills show` in TUI renders the scope table (needs a
      terminal; code path is the tested `skill_show_lines`)
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 7: Prompt surface + trust verification (SK-IMP-007)

| Field | Value |
|---|---|
| **ID** | `SK-IMP-007` |
| **Severity** | 🟡 Medium |
| **Depends on** | SK-IMP-003, SK-IMP-004 (surfaces the trust model to verify) |
| **Blocks** | Nothing |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `prompts/skills.md` (new), `prompts/system.md`, `lib/agent.cpp` (prompt loading), `tests/skill_catalog_test.cpp` (trust tests) |
| **Spec refs** | `workspace/security-model.md` [SM-09]–[SM-12], `skills/skill-files.md` [SF-01]/[SF-07], `skills/agent-skills.md` [AS-11] |

### Problem

The model needs to know skills exist (discovery block wording) and when it may
author; and the security posture must be proven by tests, not assumed.

### Target Architecture

- `prompts/skills.md`, loaded like `tools.md`, defines:
  - The discovery block format and its meaning.
  - The **explicit-request rule** for `write_skill` (never author unsolicited).
  - That activated skill bodies are advisory text, not privileged instructions.
- Trust tests (part of the suite, not a separate mode):
  - `allowed-tools` frontmatter parsed and dropped — never gates or grants
    ([SM-10], [SF-07]).
  - A malicious body cannot change tool approval/read-mode behaviour end-to-end
    ([SM-09], [AS-11]).
  - `block` with author metadata persists across catalog reconstruction
    ([SM-12], [SK-08]).

### Refactor Rules

- `prompts/skills.md` is runtime Markdown like `system.md`/`tools.md` — no
  compiled-in text.
- Trust tests exercise the real `Tool` dispatch path where feasible (approval
  hook returning deny), not just parser unit checks.

### Verification

- [x] Tests for [SM-09], [SM-10], [SM-11], [SM-12] as above
      (`skill_trust_*` in `tests/skill_catalog_test.cpp`; [SM-11] covered by
      `skill_catalog_interop_gate`)
- [x] `prompts/skills.md` loaded at session start (test asserts non-empty)
- [x] `make clean && make && make test && make lint && make analyze` clean
- [x] `grep -rn 'allowed-tools' lib/ tools/` shows only the parse-and-ignore
      site, no behavioural use (no matches — the parser drops unknown keys
      generically)

---

## Dependency Graph

```
SK-IMP-001 (drop dead fields) ── independent
SK-IMP-002 (project store + migration) ── independent
SK-IMP-003 (SKILL.md parser + scanner) ── independent
   │
   ├── SK-IMP-004 (catalog + overrides + discovery) — needs SK-IMP-002, SK-IMP-003
   │     │
   │     ├── SK-IMP-005 (skill tools) — needs SK-IMP-004
   │     │     │
   │     │     └── SK-IMP-006 (/set skills subtree) — needs SK-IMP-004, SK-IMP-005
   │     └── SK-IMP-007 (prompt + trust) — needs SK-IMP-003, SK-IMP-004
```

**Recommended execution order:**

1. SK-IMP-001 + SK-IMP-002 + SK-IMP-003 (independent, parallel friendly)
2. SK-IMP-004 (foundation for the rest)
3. SK-IMP-005
4. SK-IMP-006 + SK-IMP-007 (parallel friendly after SK-IMP-004)
