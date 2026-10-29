# Memory/learn integrity — implementation proposal

- **Issue:** #444 (validate and close gaps in the memory/learn system)
- **Branch:** `feat/memory-learn-integrity` from `main` @ `1da04e2e`
- **Status:** proposal — no production code before sign-off
- **Delivery:** one milestone PR; each numbered item below is a commit (red test
  first, green fix, docs in the same feature commit when the contract changes)

---

## 1. What the issue asserts, and what the code actually does

The issue was written from the 2026-07-29 audit. Since then the catastrophic
version of Gap 1 was fixed (the memory suffix is no longer appended to the
system prompt), the learn backends landed, and the extraction call sites were
wired. The remaining claims do not all hold, and two of them hide a regression
rather than an omission.

Measured on `1da04e2e`:

| Issue claim | Measured state | Verdict |
|---|---|---|
| Gap 1: memory injection modifies the system prompt every turn | `collect_prompt_blocks` (`lib/agent.cpp:1137`) emits a `kMemory` **head block**; `insert_prompt_blocks` (`lib/agent.cpp:1066`) inserts it *after* the system message as a separate `system` message. The system prompt at index 0 is untouched. | **Partly fixed.** The slot is right; there is no version/cache, and selection still depends on the conversation. |
| Gap 2: no end-to-end validation | `integration_apply_and_retrieve` (`tests/run_tests.cpp:7069`) applies ops by hand and calls the retriever. No test runs compress → extract → persist → inject through `Agent`. | **Open.** |
| Gap 3: `feat/learn-ui` may not be merged | No such local or remote branch exists. The work landed in `7f7fb701` and was **deleted** in the action-driven dispatch rewrite `589bc7e3` (`cmd_learn`, `cmd_learn_panel` removed; no `learn` node added to `completions.json`). `learn_show_lines`, `learn_inspect_lines`, `learn_items` are now production-dead: only tests call them. | **Regression, not a missing branch.** |
| Gap 4: `promoted = true` on creation | `apply_memory_ops` sets `evidence = threshold`, `promoted = true` (`lib/compressor_apply.cpp:588`), then `apply_compression_result` immediately decays the new item (`lib/agent.cpp:1505`). It is intentional per `extraction.md` invariant 4, but it lands one evidence point below threshold and is untested. | **Intentional; needs to be pinned by a test.** |
| Gap 5: injection ordering | Same as Gap 1: the memory block is a head block, before the conversation. Correct placement; no cache. | **Fixed placement, open guarantee.** |

Additional findings from the code trace (not in the issue):

1. **Decay does not run on a cycle with no ops.** `apply_compression_result`
   returns early when `cr.memory_ops` and `cr.skill_ops` are both empty
   (`lib/agent.cpp:1489`). `extraction.md` invariant 5 says decay is
   unconditional — absence of reconfirmation is the signal.
2. **A freshly confirmed item is decayed in the same cycle.** `decay_maps`
   (`lib/memory_scoring.cpp:156`) has no notion of "confirmed this cycle", so a
   new memory starts at `threshold` and leaves the cycle at `threshold - 1`.
3. **Name-conflict skipping is only a caller convention.** `apply_memory_ops`
   checks `find_memory(name)` and skips (`lib/compressor_apply.cpp:576`), but
   `JsonMemoryStore::upsert` itself will create a second entry with the same
   name and different content. The store contract (`memory-store.md` MS-03)
   says the store skips.
4. **`experience_enabled` is dead.** `load_experience_config` sets
   `enabled = false` (`lib/memory_store.cpp:154`), but `make_memory_store`
   ignores it and every host constructs a store anyway. Turning experience off
   does not turn it off.
5. **The configured budgets are not used.** `MemoryRetriever` hardcodes
   `top_memories(20)`, `top_skills(10)` and `500` tokens
   (`lib/memory_retriever.cpp:10`). `max_prompt_tokens` and `max_memories` are
   never read by injection; `max_skills` only bounds the one-time learned-skill
   discovery. `/get learn` therefore reports a budget the injector does not
   honour.
6. **Selection is not deterministic under ties.** `select_top_memories` sorts
   by score only (`lib/memory_scoring.cpp:87`); equal scores keep
   `unordered_map` iteration order, so a rebuild can reorder the block without
   any knowledge changing.
7. **Learned skills are re-indexed never and duplicated often.** The catalog is
   discovered once at construction (`lib/agent.cpp:101`), not after extraction,
   so a skill learned this session is not `read_skill`-able until a restart. At
   the same time the retriever injects learned skill bodies in the memory
   block, while `discovery_block` also lists them as `"(no description)"`
   because learned entries never set `meta.description`
   (`lib/skill_catalog.cpp:259`).
8. **`top_skills` contradicts its own spec.** `memory-store.md` MS-05 says
   non-matching promoted skills are the fallback; `select_top_skills` returns an
   empty list when nothing matches (`lib/memory_scoring.cpp:114`), and
   `memory_store_skill_trigger` asserts that empty result. Consequently
   `discover(store->top_skills(k, ""))` only ever indexes skills with an empty
   trigger phrase.
9. **A pinned item is not protected.** `set_promoted` is the pin flag, but
   `decay_maps` erases an item at evidence 0 regardless. `learn-ui.md`
   invariant 3 promises the opposite. With one boolean there is no way to tell
   a manual pin from automatic promotion, so the promise cannot be kept without
   a second field.

---

## 2. Design

### 2.1 The stable prefix (Gaps 1 and 5)

**Invariant:** the request prefix `[system prompt][learned-memory block]` is a
byte-identical function of the system prompt and the store state. It changes
only when the store version changes. The conversation follows it; the tail
blocks (brief, activated skills, plugin tail) may change freely.

- `MemoryStore` gains `virtual std::uint64_t version() const`, monotonic,
  bumped by every mutation that changes data (`upsert`, `remove`,
  `set_promoted`, `decay_all`, a successful `load`). `save` and
  `set_current_turn` do not bump: they change no knowledge.
- `MemoryRetriever` caches the rendered block keyed by `(version)`; a hit
  returns the previous string without re-selecting. The retriever is
  per-agent, so the cache needs no lock (single-owner, like `Context`).
- Selection becomes a pure function of the store: `top_memories(max_memories,
  "")`, ordered by evidence/freshness with a `name` tie-break. The current
  first-user-message heuristic (`lib/agent.cpp:1095`) is deleted; a
  conversation change can no longer change the block.
- The token budget and caps come from `ExperienceConfig` (`max_memories`,
  `max_prompt_tokens`), not hardcoded constants. Entries are admitted whole;
  the old `resize()` could cut a memory in half.
- **Skills decision:** learned skills are removed from the memory block and
  live in the `SkillCatalog` (index + `read_skill`), exactly like authored
  skills. This removes the double injection, keeps the prefix small, and makes
  the trigger phrase a discovery hint. See decision D1.

Tests:
- `memory_block_is_a_function_of_the_store` (**red today**): two retriever calls
  with different user messages (`"build"`, `"deploy"`) return the same block.
  Today the block follows the query.
- `memory_block_survives_a_turn_counter_change` (guard): `set_current_turn`
  without a data change leaves the block byte-identical, even when a freshness
  difference collapses into a tie.
- `memory_block_changes_after_an_upsert`: the cache invalidates.
- `system_prompt_is_never_modified`: the memory text appears in a later system
  message and never in index 0, before and after a store change.

### 2.2 Decay (findings 1 and 2)

- Remove the no-ops early return: a successful compression always decays and
  saves (a store with zero items and zero ops writes nothing).
- `decay_maps` skips items whose `last_confirm_turn == current_turn`: an item
  confirmed in this cycle has just been reconfirmed, so "absence of
  reconfirmation" cannot apply to it. A new item therefore leaves the cycle at
  `threshold`, which is what `extraction.md` MS-01 describes.

Tests:
- `compression_without_ops_still_decays` (red today).
- `freshly_confirmed_item_keeps_threshold_evidence` (red today).
- `old_item_decays_by_one_cycle`.

### 2.3 The learn surface (Gap 3 and finding 3)

Restore `/learn` as a JSON command tree, not a handler branch:

```
learn { action: core.learn }
├── show   { action: core.learn.show }    # [type|tag] or <id>
├── forget { action: core.learn.forget }
├── pin    { action: core.learn.pin }     # <id> on|off
└── panel  { action: core.learn.panel }
```

- `completions.json` gains the namespace; `tui/tui_input.cpp` registers the four
  `(action, arg)` closures. The dead `learn_*` backends become live.
- `/get learn` stops being an `if (arg == "learn")` branch inside `cmd_get`;
  its registered action calls the summary directly.
- The panel is restored from `589bc7e3^` and adapted: `ListPanel` for the rows,
  `ConfirmPanel` for forget, `info_dialog` for inspect, `p` toggles promotion.
- The command tree test asserts every node's `help`, `man` and `action`;
  a pty test seeds a store, runs `/learn show`, `/learn show <id>`,
  `/learn forget <id>` and opens/closes `/learn panel`.

`/learn pin` semantics are corrected to what the code can honour (decision D3).

### 2.4 Store-level rules and the dead flag (findings 3 and 4)

- `upsert_memory` / `upsert_skill` reject a same-name/different-content entry at
  the store, so the invariant holds for every caller; `apply_memory_ops` keeps
  its report channel.
- `make_memory_store` returns `nullptr` when `!cfg.enabled`; the three hosts
  (CLI, TUI window manager, tests) construct a retriever only when a store
  exists, and `Agent` already tolerates a null store/retriever.
- `experience_enabled=false` then really disables extraction and injection, and
  `/learn show` reports `experience store disabled` instead of loading a store.

Tests:
- `store_skips_a_name_conflict` (red today).
- `experience_disabled_makes_no_store` and an agent run that never injects.
- `[LU-08]` becomes a real disabled-config round-trip, not a null-store fixture.

### 2.5 Learned skills (findings 7 and 8)

- `apply_compression_result` refreshes the catalog after a skill op:
  `skills_->discover(promoted learned skills)`, so a skill learned this session
  is immediately `read_skill`-able.
- `select_top_skills` implements its spec: matching triggers first, remaining
  promoted skills as fallback (capped). This also fixes learned-skill indexing,
  which used `top_skills(k, "")` and silently dropped every triggered skill.
- Learned catalog entries carry a description: the trigger phrase when present,
  otherwise the first sentence/line of the content. No more
  `"(no description)"`.

Tests:
- `learned_skill_is_discoverable_and_readable_after_extraction` (red today).
- `skill_discovery_uses_the_trigger_as_its_description`.
- `top_skills_falls_back_to_promoted_skills` (the existing
  `memory_store_skill_trigger` expectation changes).

### 2.6 End-to-end proof (Gap 2)

One test builds a real `Agent` with a real `JsonMemoryStore` at a temp path, a
`MemoryRetriever`, a shared `SkillCatalog`, and a `FakeLLMClient` scripted with
a classify response and an extract response carrying one memory and one skill:

1. `compress_now()` runs the real pipeline.
2. The store contains the memory at `threshold`, promoted; the file reloads with
   the same content.
3. The skill is indexed and `read_body()` returns its content.
4. The next `run()` request is `[system][memory block][user…]`; the memory
   content is in the block and not in the system message.
5. A second compression with no ops decays the item (finding 1).

---

## 3. Commit plan

| # | Commit | Content | Red test |
|---|---|---|---|
| 0 | `docs: memory/learn integrity proposal` | this document | — |
| 1 | `memory: a versioned, stable knowledge block` | store `version()`, retriever cache, config budgets, deterministic ordering, memories-only block (D1) | `memory_block_is_a_function_of_the_store` (red), `memory_block_survives_a_turn_counter_change` (guard) |
| 2 | `memory: decay every compression and spare fresh confirmations` | remove early return, `decay_maps` skip | `compression_without_ops_still_decays`, `freshly_confirmed_item_keeps_threshold_evidence` |
| 3 | `memory: enforce conflict and enablement at the store` | store name-conflict, `enabled → nullptr`, host wiring | `store_skips_a_name_conflict`, `experience_disabled_makes_no_store` |
| 4 | `memory: index learned skills immediately` | catalog refresh, `top_skills` fallback, description | `learned_skill_is_discoverable_and_readable_after_extraction` |
| 5 | `learn: restore the /learn command surface` | `completions.json` nodes, actions, panel, `/get learn` cleanup | tree test + pty scenario |
| 6 | `test: prove compress → extract → persist → inject` | the end-to-end test | (green from the start once 1–4 land) |
| 7 | `docs: reconcile the memory specs and the learn tracker` | tracker, store path, pin, budgets, MISSION row, audit status | — |

Each behavioral commit carries its red test first; where a test changes an
existing expectation (skill fallback, decay order) the commit says why.

---

## 4. Docs to reconcile

- `docs/learn-tracker.md`: status says "complete on `feat/learn-ui`"; the work
  is in `main`, the surface was deleted in `589bc7e3`, and this milestone
  restores it. Record the commit ids.
- `docs/spec/memory/memory-store.md`: store path is per-project **state**
  (`$XDG_STATE_HOME/amber/projects/<slug>/experience.json`), not
  `<workspace>/.amber/experience.json`; add `version()` and the stable-prefix
  invariant; MS-03 conflict is now a store rule; MS-05 fallback matches code.
- `docs/spec/memory/extraction.md`: decay-every-cycle, the fresh-confirmation
  exemption, and the intentional `promoted=true` on creation (already invariant
  4, add the test reference).
- `docs/spec/memory/learn-ui.md`: pin semantics (D3); `/learn` is a tree
  namespace; the panel is reachable.
- `docs/deep-audit/context-system-analysis.md`: Phase 0.1 and 4.1 say
  "Resolved"; add what is now *measured* and by which test.
- `docs/spec/MISSION.md`: the "Memory/skill UI not wired" row and the stale
  project-scoped store row.

---

## 5. Decisions requiring sign-off

**D1 — What the stable block contains.**
Recommended: **memories only**. Learned skills move to the catalog (index +
`read_skill`), which removes the duplicate name/body injection, keeps the
prefix small, and matches the authored-skill activation model. Alternative:
keep learned skills in the block; then the block is larger and a skill is both
injected and discoverable. Either way the block is versioned.

**D2 — Where the version counter lives.**
Recommended: **in memory in the store** (monotonic `uint64_t`), not persisted.
A new process builds a fresh block anyway, so a persisted counter has no
consumer and can go stale; the issue's open question suggests persisting it,
but that buys nothing for the prefix and adds a migration.

**D3 — Pin semantics.**
Recommended: **correct the promise, not the schema**. `promoted` is the single
manual/automatic flag; `/learn pin` force-promotes now, and decay still applies.
The alternative is a distinct persisted `pinned` field with "decay skips it",
which is a schema change and changes the meaning of the existing flag. The
learn-ui spec currently promises protection the one-flag model cannot provide.

---

## 6. Out of scope

- No change to the inference service, model, or endpoint configuration.
- No prefill measurement: the configured provider reports prompt/completion
  only, so the prefix guarantee is structural (byte identity), as recorded in
  the compaction baseline.
- No relevance-ranked per-turn memory retrieval. If that is wanted later it
  belongs in a **tail** block (append-only), not in the prefix.
- No new embedding/vector store; the scoring model is unchanged.
- No multi-process store coordination beyond the existing per-project store and
  its locks.

## 7. Risks

- **Skills move (D1)** changes what a learned skill costs per turn: bodies are
  no longer resident; the model must `read_skill`. This is the designed
  authored-skill flow, but it is a behavior change and the reason it is D1
  rather than a silent choice.
- **`ExperienceConfig` budgets** now actually cap injection; a store that
  previously injected 20 memories with the hardcoded cap will follow the
  configured cap (default 20, so no change unless configured).
- **Pty tests** are the only coverage for the restored TUI glue; the patch
  coverage gate will fail if the panel is added without a scenario.
