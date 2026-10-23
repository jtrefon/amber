# Proposal: command-tree taxonomy and the host-owned alias layer

Date: 2026-10-23
Status: PROPOSAL — awaiting sign-off (step 3 of the fix workflow)
Supersedes: parts of `docs/spec/tui/command-namespace.md` (§5 migration table)
and `docs/spec/tui/command-tree.md` (2026-10-22 revision) once signed off.

## 1. Measured background (what is actually broken)

Verified against the working tree (`run_tests` resolver semantics reproduced in a
standalone probe against `completions.json`):

| Behavior | Dispatch | Drawer / completion | `?` man | Source |
|---|---|---|---|---|
| Node alias (`exit` → `quit`) | works (`/exit`, `/e`, `/q` all resolve) | absent — rows never contain alias names | absent | aliases live under node `aliases` and are consulted only in `match_child` (tui/setting_registry.cpp:463-473) |
| Root alias (`model` → `set model`) | does not exist | does not exist | does not exist | no mechanism in the tree or the registry |
| Hidden legacy map (`compress` → `session compress`) | works silently | absent | absent | `legacy_paths_` is consulted only by `expand_legacy` (tui/setting_registry.cpp:584) |
| `compress` command node | dispatches at `session.compress` with action `core.compress` | visible only after descending into `/session` (typed `/comp…` shows nothing) | visible only there | `compress` was re-homed from a root to `commands.session.children` in PR #332 (`2f12805a`) — deliberate, test-asserted (`tests/completions_test.cpp:254`) |

Three of the five surfaces regressed at 0.6.0 on purpose and were never rebuilt:

- Pre-0.6.0 `tui/completion_context.cpp` appended
  `settings.top_level_aliases()` to completion rows; the 0.6.0 rewrite dropped
  that and now documents "aliases are dispatch hints, not duplicate completion
  rows" (tui/completion_context.cpp:11-12).
- `SettingRegistry::top_level_aliases()` is now consumed by nothing but its own
  test (tests/completions_test.cpp:1492) — dead code in the UI path.
- The `legacy` map introduced in the same PR was always dispatch-only.

There is **no hardwire and no JSON override** anywhere: drawer, completion and
`?` pages are pure walks of the merged tree (verified against every feed —
provider, policy, job, plugin, worktree, MCP — contribute under `get`, `set`,
`worktree`, `job` only). The regression is a missing feature, not a hijack.

Aggravating factor: `docs/spec/tui/command-namespace.md` §5 schedules the hidden
legacy aliases for removal after one release. Executed as written, 0.7.0 would
silently kill `/compress` and every other 0.5.x muscle-memory spelling.

## 2. Standards scan (what the ecosystem does)

| Tool | Shape | Query vs mutate | Aliases |
|---|---|---|---|
| Claude Code | flat verb roots (`/model`, `/compact`, `/clear`, `/config`, `/mcp`, `/exit`) | config edits are interactive panes, not subcommands | flat duplicates; renamed commands keep old spelling dispatching |
| Codex CLI | flat verb roots (`/model`, `/approvals`, `/init`, `/review`, `/compact`) | approvals/model are own commands, not config trees | minimal |
| aider | flat prompt commands, liberal matching | `/tokens`, `/settings` for reads | unambiguous unique-prefix matching for every command |
| kubectl | `get`/`describe` read plane, `set`/`apply`/`delete` command plane, resource nouns | exactly the query/command split | shortnames (`po` ↔ `pods`), never dispatched-then-dropped |
| git | `git config get|set` for key-values (2.46+); lifecycle verbs own roots (`git mv`, `git rm`) | key-values under config get/set | compat aliases for renamed commands |

Distilled principles adopted below:

1. **The drawer is the root test.** A root list is the frequency list; ~15 rows.
2. **`get` = retrieve, `set` = request/mutate** — one plane for every
   key-value-ish surface, core and plugin alike (amber already punts plugin
   settings there via `SettingsCapability` — `docs/spec/plugins/developer-guide.md:337`).
3. **Domains own lifecycle verbs**, depth ≤ 2. `session save`, `window new`.
4. **Global verbs ride the alias layer**, not the root list: they dispatch like
   commands and are shown only when curated.
5. **One canonical spelling, one doc string.** Alias surfaces inherit the
   target's help/man; nothing is duplicated, nothing drifts.
6. **Matching stays prefix + ambiguity-guarded** (already implemented in
   `match_child`).

## 3. Taxonomy (where a command goes)

Decision table for every future command, core or plugin:

| It is… | Placement |
|---|---|
| read of config/state | `get.<group>` |
| request/mutation of config/state | `set.<group>` |
| lifecycle verb of a resource | its domain root (`session`, `window`, `worktree`, `project`, `provider`, `job`, `plugin`) |
| cross-domain global verb | alias layer (visible when curated, hidden otherwise) |
| plugin setting/state | `SettingsCapability` → generated `get/set plugin settings` leaves |
| plugin action | `/plugin <id> <verb>` |
| plugin-requested daily-use root | host allowlist grant, documented in `completions.json` |

Current roots audit (22 after this plan: 18 domains + `help`/`settings`/`quit`
+ nothing new): `help`, `settings`, `session`, `window`, `worktree`, `get`,
`set`, `files`, `system`, `provider`, `job`, `project`, `work`, `plugin`,
`panel`, `quit` stay. No root is added or removed. Deferred consolidation
candidates (recorded, not scheduled): `settings` as a lens over `get`; the
read-only halves of `system` (`info`, `ps`, `df`, `uptime`, `uname`) as a
`get system` readout; `files` stays whole.

## 4. The alias contract

### 4.1 JSON schema

The tree stays the single source; the alias layer is part of it:

```json
{ "aliases": { "model": "set model", "mcp": "plugin mcp" }
, "legacy":  { "session compress": "compress", "sl": "session list" }
, "commands": { ...unchanged... } }
```

- **`aliases` — visible.** Each entry maps a single root token `key` to a full
  canonical path. It behaves like a command at the root: dispatches, shows in
  the drawer, completes, and carries a `?` page.
- **`legacy` — hidden.** Same resolution, never surfaced. This is the existing
  map, repurposed as the home for renamed/compat spellings; entries retire
  after their grace window (the spec's removal rule finally becomes safe).
- Keys are single tokens; targets may be any depth. Multi-token keys remain
  out of scope (YAGNI), which is what makes compat spellings like
  `session compress` live as nodes, not aliases.

### 4.2 Resolution (one code path)

`expand_legacy` becomes one `resolve_alias_token(tokens)` consulted by
`resolve_command` for both maps; the consumed-tokens convention stays (the user
typed only `key`, so `consumed = 1` and the action receives the user's own
argument suffix — `/model gpt` reaches `cmd_model_set("gpt")`,
`/exit` reaches `request_quit()`).

### 4.3 Surface matrix (the standard being restored)

| Surface | Rule |
|---|---|
| Dispatch | alias token expands, deepest target node's action runs — exactly today's behavior |
| Drawer (bare `/` and first-token filter) | visible aliases are rows, ordered after real commands, rendered as `model  (→ /set model)`; they filter and prefix-match like any row; provider-tab filtering treats them like core rows (no `::`) |
| Cache/descend | `children_of("model")`, `complete("model")` and `find_namespace_node("model")` resolve through the alias target — `/model ` descends into the set-model namespace (setting rows + model feed) |
| `?` man | `man_for("model")` = target's `man`, inherited — alias pages never diverge |
| Help/readout | recorded as alias of the canonical path; no duplicate help text exists |

Invariants held: the drawer stays 1:1 with Enter dispatch (every alias row is
dispatchable through the same walker); `completions.json` stays the only
source; no C++ list appears anywhere.

### 4.4 Collision and ownership rules

- An alias key may not shadow a documented command root; the loader refuses and
  counts it (`alias_collisions()`, reported by the registry next to
  `merge_collisions()`).
- The alias maps are **core-owned**: feeds, plugin subtrees and manifest
  contributions may never add or patch them — attempts count as collisions.
- Grants live only in `completions.json` (the §4.3 "host-owned allowlist" of
  the namespace spec, finally made real).

### 4.5 Proposed visible set (curated, ≤ 10)

| Alias | Target | Why |
|---|---|---|
| `model` | `set model` | the user's standardization example; every peer app has `/model` |
| `compress` | `session compress` | the reported regression; never moves again |
| `compact` | `session compress` | already node-alias only; unify under one map |
| `stop` | `session stop` | interrupt is a panic-button verb; `cancel`/`kill` stay hidden |
| `clear` | `session new` | `/clear` is the peer-app standard for a fresh context |
| `save` | `session save` | frequency justifies |
| `sessions` | `session list` | discovers the session browser |
| `mcp` | `plugin mcp` | MCP is the established spelling ecosystem-wide |

Everything else in today's `legacy` map (`new`, `reset`, `kill`, `cancel`,
`open`, `load`, `hello`, `watch`, `prompt`, `index`, `sl`) stays hidden.

### 4.6 One open decision for sign-off

- **Option A (recommended):** `compress` remains a canonical node at
  `session.compress`; the fix is the alias layer. Root stays domain-only; the
  spec's language survives unchanged. Drawer parity is achieved through the
  visible alias.
- **Option B:** relax `NS-01` with an explicit two-item action-root class
  (`compress`, `stop`) restored as canonical roots. Muscle memory wins at the
  cost of reopening the root gate the 0.6.0 work closed.
The rest of the plan is identical under both; only the visible-alias table,
the NS-01 wording and one node move differ.

## 5. Implementation plan

### PR-1 — alias contract, registry + JSON (RED → GREEN)

First commit on branch `fix/alias-layer-command-tree` is the failing test
block (Red), then the change:

1. `SettingRegistry` — single-source alias loading: parse both maps into one
   `alias_paths_` keyed by token with a visible flag; `resolve_command`
   expands through it; visible entries are indexed into the help/man view
   (inheriting the target's text) and into top-level completion children.
   `find_namespace_node`, `children_of`, `complete` resolve alias heads.
   `top_level_aliases()` is deleted; its old call sites rely on `complete("")`
   including alias rows.
2. `completions.json` — add the `aliases` block per §4.5; `legacy` keeps the
   rest. No node moves (Option A).
3. `merge_*` — alias maps are not merge paths; contributions writing them are
   refused and counted.
4. `drawer_rows` / `completion_context` — alias rows render with the
   `(→ /path)` hint; filtering and Enter need no change (they already route
   through the registry).

Tests (all in `tests/completions_test.cpp` unless stated):

- RED-1 `resolve_command({"model","gpt"})` → action `core.config.set.model`,
  `consumed == 1` (currently NULL).
- RED-2 `complete("")` contains `model` (dispatcher's `build_commands`
  inherits this — the drawer row comes from the same walk).
- RED-3 `children_of("model") == children_of("set.model")`; the drawer
  descends into the model feed's rows.
- RED-4 `man_for("model") == man_for("set.model")`.
- RED-5 `/exit`, `/compact` still dispatch (regression guard for today's
  working dispatch), and `complete("")` contains alias rows.
- RED-6 hidden legacy entries never appear in `complete("")`.
- RED-7 an alias key equal to a documented root is refused, counted, and
  leaves the root intact.
- RED-8 a feed/plugin subtree attempting an alias write is refused and counted.
- RED-9 `merge_collisions()` stays 0 for normal feed refreshes (guard).
- PTY: bare `/` shows `model (→ /set model)`; Enter-on-`/model ` descends;
  `?` page shows the target man (tests/tui_tests.cpp + pty harness).

Gates: `make && make test`, `make lint`, `make analyze`, `make duplicates`,
`make complexity`, `make nesting`, coverage ≥ 80% on the added lines.

### PR-2 — docs sync

- `docs/spec/tui/command-tree.md`: alias row in the Consumer table; §Plugins
  keeps "grants live in completions.json".
- `docs/spec/tui/command-namespace.md`: §4.3 becomes "host-owned grants are
  the `aliases` map"; §5 migration table stops promising deletion, points at
  `legacy` retention windows; NS-05 updated (hidden aliases dispatch, visible
  aliases are drawer peers).
- `AGENTS.md` command-tree section: the alias-maps paragraph.
- `website` manual + install command tables re-sync (`/compress` visible
  again).

### PR-3 (optional, separate review) — deferred consolidation

`settings`-as-lens, `system` readout split, `watch`/`prompt`/`index`
retirement windows. Each its own proposal when it makes sense, per the
decision table in §3.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Drawer noise | visible set ≤ 10, reviewed each release |
| Prefix ambiguity pool widens | existing ambiguity guard reports candidates instead of guessing |
| Two maps, drift between surfaces | one resolution function; the surface matrix is unit-tested, not aspirational |
| Plugin/alias interaction | alias maps core-owned, merge-refused (RED-8) |
| Legacy removal resurrects silent deaths | visible/hidden distinction is explicit in JSON; nothing dispatches without documentation |

## 7. Out of scope

- Multi-token alias keys; option B root relaxations beyond `compress`/`stop`;
  CLI-side completion (headless client has no drawer); any change to
  `lib/` — the work is `tui/setting_registry` + JSON + docs.

## 8. Status

IMPLEMENTED (2026-10-23, issue #345).

- GREEN lands as: alias maps in `completions.json` (visible `aliases` + hidden
  `legacy`, `clear` promoted to visible), loader book-keeping with counted
  refusals (`shadowing a root`, `dual-booking with legacy`, `not a string`),
  same-key merge refusal for feed/plugin subtrees, `resolve_key` alias probe
  after the exact index and before `get.`/`set.`, drawer/completion surfacing
  with the `(→ /canonical path)` hint (dot→space render of the canonical
  path), dispatch expansion via the shared `expand_alias` helper
  (visible then legacy).
- `mcp` → `plugin mcp` resolves through the runtime plugin feed; the unit
  contract tests the drawer row, not a static node (the plugin domain has no
  static children).
- `top_level_aliases()` (dead since the drawer dropped alias rows) is removed;
  its only caller was a test.
