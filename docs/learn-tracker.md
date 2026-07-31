# amber — Learn UI Implementation Tracker

- **Status:** 🟢 Complete — LI-IMP-001..005 implemented on `feat/learn-ui`, all gates green
- **Reference:** `docs/spec/memory/learn-ui.md`
- **Issues register:** `docs/issues.md`

---

## How to Use This Tracker

1. Every task follows the **Red → Proposal → Sign-off → Green → PR** workflow
   (see AGENTS.md). On the branch `feat/learn-ui`:
   - **Red**: Write a failing test first (scenario IDs below map to the spec),
     commit it so CI shows the failure.
   - **Proposal**: Link the task below in the PR description.
   - **Sign-off**: Reviewer approves the proposed architecture.
   - **Green**: Implement; make the test pass; refactor to zero debt
     (classes ≤200 lines, methods ≤10 lines, SOLID, hexagonal boundaries).
   - **PR**: Open/update. All checks must pass.
2. Each task is **self-contained** and ordered by dependency.
3. **Verification** before marking a task `[done]`:
   `make clean && make && make test && make lint && make analyze`.
4. No comments that restate code; first line of new files is functional.
   Run `clang-format -i` on touched files.
5. Spec scenarios are the acceptance contract; test names reference them
   (e.g. `[LU-01]`).

## Legend

```
[ ] — Not started   [~] — In progress   [x] — Done, all checks pass   [!] — Blocked
```

---

## Task 1: Store list/remove/promote APIs (LI-IMP-001)

| Field | Value |
|---|---|
| **ID** | `LI-IMP-001` |
| **Severity** | 🟠 High |
| **Depends on** | None (pure store work) |
| **Blocks** | LI-IMP-002, LI-IMP-003 |
| **Estimated effort** | 3-4 hours |
| **Files touched** | `include/agent/experience.h`, `lib/memory_store.cpp`, `tests/run_tests.cpp` |
| **Spec refs** | `memory/learn-ui.md` (store API additions), [LU-01]/[LU-03]/[LU-04]/[LU-05] |

### Problem

The `MemoryStore` port has relevance-ranked `top_*` and name-based `find_*`
queries but no enumeration, deletion, or promotion control — the learn UI's
primitives don't exist.

### Target Architecture

- `all_memories()` / `all_skills()`: full listings sorted by `score`
  descending (relevance to an empty query → evidence/recency-driven).
- `remove(id)`: erase from either collection (content-hash ids are unique
  across collections in practice); returns `true` when found.
- `set_promoted(id, pinned)`: flip the flag; returns `false` when unknown.
- Defaulted virtuals on the port so existing call sites compile untouched.

### Refactor Rules

- Ranking logic is reused, not duplicated (extract the score helper if needed).
- No changes to the `top_*`/`find_*` paths or the JSON format.

### Verification

- [x] Red tests: `[LU-01]` listing order, `[LU-03]` remove+persist round-trip,
      `[LU-04]` unknown-id remove, `[LU-05]` set_promoted persists
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 2: Agent learn accessors + wrappers (LI-IMP-002)

| Field | Value |
|---|---|
| **ID** | `LI-IMP-002` |
| **Severity** | 🟠 High |
| **Depends on** | LI-IMP-001 |
| **Blocks** | LI-IMP-003 (backends need save semantics) |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `include/agent/agent.h`, `lib/agent.cpp`, `tests/run_tests.cpp` |
| **Spec refs** | `memory/learn-ui.md` (Agent accessors), [LU-03]/[LU-05]/[LU-08] |

### Problem

The UI has no access to the session store, and persistence must stay inside
the core (the UI must never know the store path).

### Target Architecture

- `memory_store()` accessors (nullptr when experience is disabled).
- `learn_forget(id)` / `learn_pin(id, pinned)`: store op + save to
  `experience_cfg_.store_path`; typed error strings (`no learned item with id
  '…'`, `experience store disabled`).

### Refactor Rules

- The wrappers are the only UI-facing mutation path; no store path leaks.

### Verification

- [x] Red tests: forget persists via a real Agent round-trip; pin persists;
      disabled store errors ([LU-08])
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 3: Command backends (LI-IMP-003)

| Field | Value |
|---|---|
| **ID** | `LI-IMP-003` |
| **Severity** | 🟠 High |
| **Depends on** | LI-IMP-001, LI-IMP-002 |
| **Blocks** | LI-IMP-004 (TUI glue) |
| **Estimated effort** | 3-4 hours |
| **Files touched** | `include/agent/learn_commands.h` (new), `lib/learn_commands.cpp` (new), `tests/learn_commands_test.cpp` (new, add to `UNITTEST_OBJ` in `Makefile.in`) |
| **Spec refs** | `memory/learn-ui.md` (backends + formatting contract), [LU-01]/[LU-02]/[LU-06]/[LU-07]/[LU-09] |

### Problem

The TUI needs testable formatting/filtering logic without ncurses.

### Target Architecture

- `learn_show_lines(store, filter)` — table lines with the exact formatting
  contract from the spec; `memory`/`skill`/tag filters; `(no learned items)`
  and `experience store disabled` handling.
- `learn_inspect_lines(store, id, error&)` — detail page incl. trigger phrase
  for skills.
- `learn_summary_lines(store, cfg)` — counts + budget usage + store path
  (path from the ExperienceConfig passed in, not from the store).

### Refactor Rules

- Pure functions over the `MemoryStore` port — no Agent dependency in the
  formatting layer.

### Verification

- [x] Red tests: [LU-01], [LU-02], [LU-06], [LU-07], [LU-09] line-by-line
      against the formatting contract
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 4: TUI wiring — /learn, /get learn, panel (LI-IMP-004)

| Field | Value |
|---|---|
| **ID** | `LI-IMP-004` |
| **Severity** | 🟡 Medium |
| **Depends on** | LI-IMP-003 |
| **Blocks** | Nothing |
| **Estimated effort** | 4-5 hours |
| **Files touched** | `tui/tui_input.cpp` (`cmd_learn`, `/get learn`), `tui/tui.h`, `tui/tui_session.cpp` (panel) |
| **Spec refs** | `memory/learn-ui.md` (surfaces), [LU-01]–[LU-09] |

### Problem

No user-facing surface exists.

### Target Architecture

- `cmd_learn` (show/inspect/forget/pin/panel) following the string-based
  command pattern (consistent with skills/MCP until the CommandNode tree
  lands); `/get learn` summary branch in `cmd_get`.
- The panel reuses the session-list pattern (filter box + key actions) and
  shows evidence bars; `d` = forget behind the existing ConfirmPanel,
  `p` = pin, Enter = inspect via info_dialog.
- All handlers are thin glue over the LI-IMP-003 backends and LI-IMP-002
  wrappers (the agent comes from `win().agent`).

### Refactor Rules

- No formatting logic in the TUI (everything comes from the backends).

### Verification

- [x] Manual: `/learn show`, `forget`, `pin`, `/get learn` in the TUI with a
      real store; panel actions
- [x] `make clean && make && make test && make lint && make analyze` clean

---

## Task 5: Docs + final gate (LI-IMP-005)

| Field | Value |
|---|---|
| **ID** | `LI-IMP-005` |
| **Severity** | 🟡 Medium |
| **Depends on** | LI-IMP-004 |
| **Blocks** | Nothing |
| **Estimated effort** | 1-2 hours |
| **Files touched** | `docs/spec/INDEX.md`, `docs/spec/MISSION.md` (gap register row) |
| **Spec refs** | — |

### Verification

- [x] INDEX.md lists `memory/learn-ui.md`; MISSION.md gap row updated
- [x] Final: `make clean && make && make test && make lint && make analyze`
      clean; tracker closed

---

## Dependency Graph

```
LI-IMP-001 (store APIs)
   └── LI-IMP-002 (agent wrappers)
          └── LI-IMP-003 (command backends)
                 └── LI-IMP-004 (TUI wiring)
                        └── LI-IMP-005 (docs + gate)
```

## Scenario → task map

| Scenario | Task |
|---|---|
| [LU-01] show lists | LI-IMP-001, LI-IMP-003 |
| [LU-02] inspect | LI-IMP-003 |
| [LU-03] forget persists | LI-IMP-001, LI-IMP-002 |
| [LU-04] unknown id | LI-IMP-001, LI-IMP-002 |
| [LU-05] pin persists | LI-IMP-001, LI-IMP-002 |
| [LU-06] filters | LI-IMP-003 |
| [LU-07] empty store | LI-IMP-003 |
| [LU-08] disabled store | LI-IMP-002, LI-IMP-003 |
| [LU-09] budget summary | LI-IMP-003 |
