# Spec: Slash shortcuts and prefix resolution

- **Status:** Build contract for 0.6.0. Decisions locked 2026-10-22.
- **Tracking:** #319; resolver #323; completion UX #324; release #331.
- **Related:** `command-namespace.md`, `command-tree.md`,
  `input-system/slash-engine.md`, `input-system/nested-commands.md`.

## 1. Current regression

The shortcut system is half-present:

- `completions.json` declares `aliases`; the drawer and help display them.
- `SlashDispatcher::handle_slash()` walks `command_tree()` by exact child name.
  It never resolves an alias, so `/q`, `/win` and `/sl` do not dispatch.
- `descend_command_tree()` is exact-only. There is no prefix acceptance and no
  glued path resolution.
- `slash-engine.md` SE-17..SE-26 and `nested-commands.md` describe the desired
  behavior. They are acceptance criteria, not current behavior.
- The old alias table in `nested-commands.md` is stale (it references removed
  roots such as `/model`, `/files`, `/save`). It must not be copied. Shortcuts
  are generated from the canonical tree after the namespace migration.

This spec restores the behavior deterministically; it does not keep the old
first-match conflict rule.

## 2. Data model

A command node has:

- `name`: canonical path segment.
- `aliases`: full alternate names accepted as that segment (`win`, `exit`,
  `load`).
- `short`: the compact token used in a glued path (`w`, `c`, `l`).

`short` may be absent. The loader derives it from the first letter of `name`.

`short` rules:

- one or two lowercase letters by default;
- unique among siblings; conflicts extend to the shortest unique prefix of
  `name`;
- host may override any `short` in `completions.json`;
- plugin-contributed shorts are scoped under the plugin workspace and are
  validated by the host;
- a plugin cannot claim a root short or rewrite a documented short.

`aliases` are full tokens, not concatenation pieces. `short` is the only piece
used by a glued path. This separation is deliberate: `/win close` is readable;
`/wc` is compact; neither has to parse the other.

## 3. Shortcut index and conflict policy

Rebuild a global `ShortcutIndex` whenever the command tree is loaded or merged
(`SettingRegistry` reindex / plugin merge / feed merge).

For every namespace and leaf, concatenate the `short` of each path segment:

```text
window + close        -> wc
session + list        -> sl
set + model + ...     -> sm...
```

The index maps each exact shortcut to exactly one path.

Conflict handling:

1. If two paths produce the same shortcut, the loader reports a collision.
2. The host resolves it by assigning a longer `short` to one node (shortest
   unique prefix), or by an explicit `short` in JSON. A documented core short
   wins over a plugin contribution.
3. A plugin contribution that still collides is reachable by its full
   `/plugin <id> ...` path; its shortcut is disabled, not silently attached to
   another command.
4. Never dispatch a first match when a shortcut is ambiguous.

The loader exposes a `shortcut_collisions()` count for tests and the `/help`
diagnostic. Zero collisions is the release gate.

## 4. Resolution algorithm

Input is a slash command line. Only the portion before the first argument space
is a path candidate; arguments are never shortcut-parsed.

At each level:

1. Exact child `name`.
2. Exact child `alias`.
3. Exact glued `short` path (whole remaining token).
4. Unique prefix of a child `name`, `alias`, or `short` at this level.
5. Ambiguous prefix → open the drawer with candidates; do not dispatch.
6. Unknown → status error with `/help` hint.

If the resolved node has children and the input ends, it is a namespace: open
the drawer or show help; do not execute. If it has an action, dispatch it. If it
is a leaf with a free-form argument, the remaining space-separated text is the
argument.

Examples:

| Input | Expands to | Notes |
|---|---|---|
| `/wc` | `/window close` | exact leaf |
| `/sl` | `/session list` | exact leaf |
| `/smrlm` | `/set model reasoning limit max` | segments where those nodes exist |
| `/w` | ambiguous `window` / `work` / `worktree` | drawer, no dispatch |
| `/w c` | `/window close` | readable spaced path |
| `/wc 3` | no action; `close` takes no id | argument is passed to the handler |
| `/jk 3` | `/job stop 3` | argument after space |
| `/pl wl` | `/plugin watch list` | plugin workspace after migration |

Prefix acceptance is Cisco-style but must be **unique**. It never guesses.

## 5. Completion and display behavior

- The drawer at `/` lists canonical roots only, not every alias as a duplicate
  row.
- A root row may show its short: `window (w)`, `work (wk)`, `worktree (wr)`.
- Aliases are shown in the `?` help / drawer line, but are not separate rows.
- Typing a short or an unambiguous prefix shadows the **canonical full path**;
  Tab accepts the expansion. This teaches the canonical spelling without hiding
  the shortcut.
- `Enter` accepts an exact shortcut or unique prefix. Ambiguity opens the drawer.
- `/help` includes a generated shortcut table; no hand-maintained table.
- Root ordering remains priority/registration based; the shortcut hint is part
  of the row.
- Fuzzy/substring matching is not added to the command path. It would add the
  same noise that shortcuts remove.

## 6. JSON and framework changes

- Add `short` to the node fields read by `SettingRegistry`.
- Include `short` in the immutable documented field set for merges.
- Add a shortcut index to the command index. Rebuild it on every merge.
- The shortcut table is data, not a C++ list.
- `SettingRegistry`/`CommandResolver` exposes:
  - `resolve(tokens) -> {node, consumed, ambiguity, candidates}`;
  - `shortcut_for(path)`;
  - `shortcut_collisions()`.
- `SlashDispatcher` uses the resolver; it does not parse command names in
  handlers.
- Completion context uses the same index for shadow/drawer rows, so painted
  rows and dispatch can never disagree.

## 7. Acceptance tests

- **SC-01** `/q` dispatches `quit` through the alias index.
- **SC-02** `/wc` dispatches `window close` through the glued index.
- **SC-03** `/smrlm` resolves the canonical long path (where those segments
  exist), including a two-letter conflict resolution.
- **SC-04** `/w` is ambiguous and opens the drawer; it does not dispatch the
  first match.
- **SC-05** `/w c` and `/wc` dispatch the same command.
- **SC-06** arguments after a space are not shortcut-parsed.
- **SC-07** an exact short beats a prefix of another name; a full name beats a
  short of another node.
- **SC-08** a plugin cannot register a root short; a colliding plugin short is
  disabled and the full `/plugin` path still works.
- **SC-09** the drawer shows one row per command, with shortcut hints, not one
  row per alias.
- **SC-10** the shortcut index is rebuilt after a plugin/feed merge; stale rows
  and stale actions disappear.
- **SC-11** `shortcut_collisions() == 0` for the documented core tree in CI.
- **SC-12** `?`/help shows the generated shortcut table and the full canonical
  path.

## 8. Migration

- Add `short` values to canonical nodes before enabling the resolver.
- Generate the alias/shortcut table from the new tree; do not resurrect the old
  hand-written table.
- Keep legacy full aliases (`/win`, `/sessions`, `/compact`) as accepted tokens
  for one release; new code should add `short`, not another root alias.
- Remove compatibility aliases after the 0.6.x migration window.
