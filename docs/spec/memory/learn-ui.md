## Spec: Learn UI — Memory/Skill Visibility and Management

### Purpose

Give the user a first-class surface for what the agent has *learned*. Today
compression extracts memories and learned skills into the project experience
store invisibly — there is no way to see what the agent "knows", how confident
it is, what will decay, or to correct something wrong. This spec defines the
`/learn` command namespace, the `/get learn` summary, and the inspection
panel: the management surface for the learning subsystem, mirroring how
`/set skills` manages authored skills.

### Ownership

- **Source files** (target): `include/agent/experience.h` +
  `lib/memory_store.cpp` (store list/remove/promote APIs), `include/agent/agent.h` +
  `lib/agent.cpp` (learn accessors + forget/pin wrappers),
  `include/agent/learn_commands.h` + `lib/learn_commands.cpp` (command
  backends), `completions.json` (the `learn` namespace), `tui/tui_input.cpp`
  (`cmd_learn_*`, `/get learn`)
- **Test files**: `tests/run_tests.cpp` (store + agent ops),
  `tests/learn_commands_test.cpp` (backends), `tests/completions_test.cpp`
  (tree nodes), `tests/tui_pty_test.cpp` (the surface in a real terminal)
- **Spec status**: implemented. The `learn` namespace is the command tree's
  single source of truth; the handlers are `(action, arg)` closures.

---

### Surfaces

| Surface | What it does |
|---|---|
| `/learn show [type\|tag]` | Table of all learned items: `id · type · name · evidence · score · promoted · turn`. Optional filter: `memory`, `skill`, or a tag substring. |
| `/learn show <id>` | Full detail: content, tags, evidence, score, promoted flag, last-confirm turn, trigger phrase (skills). |
| `/learn forget <id>` | Remove one item from the store and persist. Errors on unknown id. |
| `/learn pin <id> on\|off` | Force-promote (or clear) an item and persist. A pin is a manual promotion, not immortality: decay still applies until the item is reconfirmed. |
| `/learn panel` | TUI list panel: filter box, evidence column, Enter opens the actions (inspect, forget with a confirmation, promote/unpromote, close). |
| `/get learn` | Summary: item counts, promoted counts, budget usage (`top_memories`/`top_skills` caps), store path. |

All surfaces are **read-only by default**; the only mutating actions are the
explicit `forget` and `pin` commands. There is no bulk clear in v1 (the store
is per-project state and rebuilds from interaction).

---

### Store API additions (`MemoryStore` port)

The current port (see `memory/memory-store.md`) has relevance-ranked
`top_*` queries and name-based `find_*`, but no enumeration, deletion, or
promotion control. The learn UI needs:

| Method | Contract |
|---|---|
| `all_memories()` | Every memory, sorted by `score` descending. Empty when the store is empty. |
| `all_skills()` | Every learned skill, sorted by `score` descending. |
| `remove(id)` | Erase the item with that id from **either** collection (ids are content-hash based; a given id exists in at most one). Returns `true` if found and removed. |
| `set_promoted(id, pinned)` | Set/clear the promoted flag on the item with that id. Returns `false` when unknown. |

`score` is the same scoring used by `top_*` (evidence weight + relevance);
for the listing, relevance to an empty query is used so the ranking is
evidence/recency-driven and stable.

### Agent accessors (`Agent`)

| Method | Contract |
|---|---|
| `memory_store()` / `const memory_store() const` | The session store (nullptr when experience is disabled). Read-only use by the UI; mutation through the wrappers below. |
| `learn_forget(id)` | `store->remove(id)` + save to `experience_cfg_.store_path`. Returns "" on success or an error string (`unknown id`, `store disabled`). |
| `learn_pin(id, pinned)` | `store->set_promoted(id, pinned)` + save. Same error contract. |

The wrappers own the **save path** so the UI never needs to know the store
location; persistence uses the store's existing atomic tmp+rename save.

---

### Command backends (`lib/learn_commands.cpp`)

Pure, testable free functions (the TUI handlers are thin glue, same pattern
as `mcp_commands.cpp` / `skill_commands.cpp`):

| Function | Returns |
|---|---|
| `learn_show_lines(store, filter)` | Table lines (or `(no learned items)` / `experience store disabled`) |
| `learn_inspect_lines(store, id, error&)` | Detail lines; error for unknown id |
| `learn_summary_lines(store, cfg)` | `/get learn` summary lines |

Formatting contract (stable for tests):

```
f3a2c1 · memory · project uses make · evidence 3 · score 1.62 · promoted · turn 12
a9b0d2 · skill  · run-tests · evidence 5 · score 2.53 · - · turn 3 · trigger "run the tests"

The state field is `promoted` when set, `-` otherwise. `score` is the store's
own metric (`MemoryStore::score_of`, evidence/recency-driven) so the shown
number always matches the listing order.
```

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Slash commands, `/get learn`, panel keys |
| **Output** | Tables, detail pages, summary, persisted store changes, panel |
| **Error states** | Unknown id → `no learned item with id '<id>'`; disabled store → `experience store disabled`; empty store → `(no learned items)` |
| **Invariants** | See below. |
| **Thread safety** | Store mutated only on the agent thread (compression, learn wrappers); the UI reads snapshots (`all_*` returns copies). |

### Invariants

1. **Read by default.** `/learn` never mutates; only `forget`/`pin` do, and
   both are explicit one-item operations.
2. **Forget is persistent and narrow.** `remove(id)` + save; nothing else in
   the store is touched. A reloaded store reflects the deletion.
3. **Pin is a manual promotion, not immortality.** `set_promoted` flips the one
   flag; decay still reduces evidence and removes the item like any other. The
   single flag cannot distinguish a manual pin from automatic promotion, so a
   distinct persisted `pinned` field was considered and deliberately not added.
   The command's help says exactly this.
4. **The UI never knows the store path.** All persistence goes through the
   `Agent` wrappers.
5. **The injected block is stable; the listing is separate.** `all_*` is the
   `/learn` listing path (score-sorted). The prompt block uses `top_memories`
   at an empty query and is cached by the store version; learned skills are
   catalog entries, not block content (see `extraction.md` ME-09/ME-10).

---

### Scenarios

#### [LU-01] /learn show lists every learned item

- **Given**: store with 2 memories + 1 skill (varying evidence/promoted)
- **Input**: `/learn show`
- **Expected**: 3 table lines, each `id · type · name · evidence · score · promoted · turn`; sorted by score desc.
- **On failure**: Items missing or relevance-ranked by an unrelated query.

#### [LU-02] /learn show <id> inspects one item

- **Given**: memory `f3a2c1` with tags and content
- **Input**: `/learn show f3a2c1`
- **Expected**: Content, tags, evidence, score, promoted, turn. For a skill, the trigger phrase is included.
- **On failure**: Unknown id → `no learned item with id 'f3a2c1'`.

#### [LU-03] /learn forget removes and persists

- **Given**: store with item `f3a2c1` on disk
- **Input**: `/learn forget f3a2c1`
- **Expected**: `removed f3a2c1`; store file rewritten without it; a fresh
  `load()` + `all_memories()` does not contain it.
- **On failure**: Deletion lost on reload (save path wrong).

#### [LU-04] /learn forget unknown id

- **Given**: no item `zzz`
- **Input**: `/learn forget zzz`
- **Expected**: `no learned item with id 'zzz'`; store unchanged (file mtime/content identical).
- **On failure**: Silent success or partial deletion.

#### [LU-05] /learn pin survives reload

- **Given**: skill `a9b0d2` unpromoted
- **Input**: `/learn pin a9b0d2 on`, then reload the store
- **Expected**: `promoted=true` after reload; `/learn pin a9b0d2 off` reverts.
- **On failure**: Pin is session-only.

#### [LU-06] /learn show skill filters

- **Given**: mixed store
- **Input**: `/learn show skill` (and `/learn show memory`, `/learn show <tag>`)
- **Expected**: Only matching items; a filter that matches nothing reports `(no learned items)`.
- **On failure**: Filter silently ignored.

#### [LU-07] Empty store

- **Given**: fresh project, no extraction yet
- **Input**: `/learn show`
- **Expected**: `(no learned items)`; `/get learn` shows zero counts.
- **On failure**: Ugly empty table or crash.

#### [LU-08] Experience disabled

- **Given**: `experience_enabled=false` (no store constructed)
- **Input**: `/learn show`
- **Expected**: `experience store disabled`.
- **On failure**: Null deref or silent nothing.

#### [LU-09] /get learn budget summary

- **Given**: store with 3 memories, `experience_max_memories=20`
- **Input**: `/get learn`
- **Expected**: `memories: 3/20 · skills: 0/10 · promoted: 1 · path: <store>`.
- **On failure**: Wrong caps or missing path.

---

### Cross-references

- **Depends on**: `memory/memory-store.md` (store port + scoring),
  `memory/extraction.md` (what evidence/promoted mean),
  `docs/spec/tui/input-system/nested-commands.md` (command tree target),
  `docs/spec/MISSION.md` (gap register: "Memory/skill UI not wired")
- **Depended on by**: `docs/spec/INDEX.md`, `docs/learn-tracker.md`
- **Test coverage**: `tests/run_tests.cpp` (store + agent ops),
  `tests/learn_commands_test.cpp` (backends)

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-31 | Initial spec (read-only default; forget/pin as the only mutations; backends in lib for testability) |
| 2026-10-29 | `/learn` restored as a `completions.json` namespace (#487); panel uses an action menu; pin semantics corrected to "force-promote, decay still applies"; filter misses report `(no learned items)` |
