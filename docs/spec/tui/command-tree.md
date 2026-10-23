## Spec: Command Tree (the JSON mapper contract)

### Purpose

One file owns the slash-command surface. `completions.json` defines every
command, its children, help/man, aliases, choices, ranges and action; the
drawer, the completion engine, the `?` help page and dispatch are all views of
that tree. Code supplies behaviour (getters, setters, handlers), never shape.
This is the rule that keeps the surface consistent as plugins and live
integrations grow: there is no second list to update and no code path that can
silently disagree with the file.

### Ownership

- **Source files**: `completions.json` (the mapper), `tui/setting_registry.{h,cpp}`
  (the only loader, index and merger), `tui/slash_dispatcher.cpp` (dispatch),
  `tui/feed_manager.cpp` + `tui/plugin_feed.cpp` + `lib/mcp_tools.cpp` (feeds).
- **Tests**: `tests/completions_test.cpp` (tree, merge, collisions),
  `tests/tui_pty_test.cpp` (drawer and dispatch end to end).

---

### The single source rule

`SettingRegistry::load_completions_json()` is the only loader. It is found
through `data_file_candidates("completions.json")`, so the file can ship beside
the binary or in the installed data directory; there is no compiled fallback.

| Consumer | Derived from |
|----------|--------------|
| Drawer rows and completion | `complete(prefix)` / `children_of(node)` (tree walk) |
| Short help | `help_for(key)` (JSON `help`), via the index rebuilt from the tree |
| Manual page (`?`) | `man_for(key)` (JSON `man`) |
| Dispatch | the deepest documented node's `action`, walked by `handle_slash` |
| Choices / ranges | `choices_for` / `range_for` (JSON `choices`, `range`) |
| Aliases | JSON `aliases` |

**Code binds behaviour only.** A `Setting` is a dotted key plus a getter and a
setter. Its help, placeholder, type, choices and range live in the mapper file;
`add_setting()` takes no metadata. A handler is a pure `(action, arg)` closure
registered by `register_action`, referenced by an `action` string in the tree.

### What is forbidden

- Hardcoded command paths or command-name branches in handlers.
- C++ completion lists, choice lists, usage hints, or drawer rows.
- A second metadata source in code (help/placeholder/type/range, a parallel
  command table, a `std::map` of commands).
- Writing `tree_` directly, renaming nodes, or overlaying fields after load.
- Patching an existing namespace from a plugin; a plugin choosing or adding a
  root. Plugin commands mount under the host-owned `/plugin/<id>` workspace.
- A second mapper file or a code-generated tree that bypasses the loader.

### Placement and root policy

Every root is a **domain**, not a feature. The standard is
`docs/spec/tui/command-namespace.md`; this file is the mapper contract that
implements it.

| Need | Placement |
|---|---|
| Plugin setting/state | `SettingsCapability` → `/get plugin settings <id> <key>`, `/set plugin settings <id> <key> <value>` |
| Plugin action | `/plugin <id> <verb>` |
| Plugin tool | registered tool; no command surface |
| Panel/status/prompt | its capability; no command root |
| First-class daily domain | host-owned root allowlist; never plugin-chosen |
| Core feature/action | child of its domain, never a new root |

Roots such as `session`, `window`, `job`, `agent`, `watch`, `project`,
`worktree`, `work`, `plugin`, `get`, `set`, `settings`, `provider`, `files`,
`system`, `panel`, `help` and `quit` are the closed domain set. Root spellings
that point at the domains they dispatch into (`model`, `compress`, `compact`,
`stop`, `clear`, `save`, `sessions`, `mcp`) are **host-granted visible
aliases**: drawer and completion rows beside the domains, resolved and
dispatched through their canonical paths, and authored only by `completions.json`
(the `aliases` map). Retired action roots (`new`, `close`, `reset`, `cancel`,
`kill`, `open`, `load`) remain **hidden legacy aliases** (the `legacy` map):
dispatch-only, never drawer or completion rows.

---

### Dynamic namespaces

Runtime content (models, providers, policy rules, jobs, plugins, MCP servers,
and the worktree registry's names) is injected as **completions.json-shaped
JSON subtrees**:

```cpp
nlohmann::json subtree = nlohmann::json::object();
subtree["set"]["children"]["provider"]["children"][name]["action"] = action;
subtree["set"]["children"]["provider"]["children"][name]["help"] = "switch to this provider";
register_action(action, handler);
settings.merge_completions_json(subtree);
```

A feed registers only the leaf action closures; it never builds a completion
row list in C++.

The same rule covers the workspace manager: `completions.json` documents the
`worktree` verbs (`new`, `attach`, `detach`, `remove`, `status`, `list`,
`prune`, `main`), and `FeedManager::refresh_worktree_feed()` merges a subtree
whose `worktree.attach.children.<name>` and `worktree.remove.children.<name>`
leaves carry host-derived actions (`core.worktree.attach.<name>`). The feeder
reads the registry, so a name appears in the drawer as soon as it exists and
disappears when it is removed; no C++ command name or choice list is involved.
Registry-only fields (name, path, parked/claimed, directory present) go in the
feed; branch, cleanliness and reachability are inspected by `/worktree status`
and `/worktree list`, so a startup feed never forks git.

#### Merge semantics

`merge_completions_json()` is additive and collision-checked:

1. **Documented fields are immutable.** A node path that `completions.json`
   supplied may not have `action`, `help`, `man`, `aliases`, `short`, `choices`
   or `range` rewritten. The documented value wins and the attempt increments
   `merge_collisions()`.
2. **Children are concatenated.** A documented node may gain new children; a
   documented command may also be a namespace (`/set model` is the switch and
   the model list, `/job kill` gains job ids). A new child path is dynamic.
3. **Dynamic nodes refresh.** Fields on a node that was introduced by a merge
   (the provider rows, job ids, plugin ids) may be updated by a later merge, so
   state text stays current.
4. **The index is rebuilt from the merged tree** after every merge. A blocked
   overwrite cannot leak into `help_for`, `choices_for` or `range_for`.

`merge_collisions()` is the collision report: zero for ordinary feed refreshes,
non-zero when a contribution tried to rewrite documented material.

#### Plugins

- **External plugins** supply `completion` in `manifest.json`; **bundled
  plugins** supply `CommandSpec::subtree`. Both are JSON subtrees, not code
  lists, and both are mounted under the host-owned `/plugin/<id>` workspace.
- A plugin does not name or add a root. It may request a root alias; the host
  grants one only through the documented root allowlist. The plugin's commands
  remain reachable under `/plugin/<id>`.
- The host derives every leaf action as `plugin.<id>.<path>`, so a plugin
  cannot name (or overwrite) a core action.
- A contribution merges **additively**, like a feed. It may extend a documented
  namespace with new children (the MCP plugin's `/get mcp` is the precedent)
  and may add children inside its workspace. It may not rewrite a documented
  `action`/`help`/`man`/`aliases`/`short`/`choices`/`range`: the documented
  value wins and the attempt increments `merge_collisions()`.
- A contribution that names the top-level `aliases` or `legacy` map is refused
  the same way and counted in `alias_collisions()`: the alias maps are
  host-owned and only `completions.json` may author them.
- Handlers are code, but they bind only the leaves their subtree declares. A
  leaf with no handler is documented and inert.

---

### Adding a command

1. Add the node to `completions.json` (nested under `children`), with `action`,
   `help`, and `man`; add `choices`/`range`/`aliases`/`short` where they apply.
   `short` is the compact glued-path token; `aliases` are full alternate names.
2. Register the action handler (`register_action`) with a pure `(action, arg)`
   closure. Do not parse the command name in the handler.
3. If the leaf comes from runtime state, add a feed that builds the JSON
   subtree and merges it (test it in `tests/completions_test.cpp`).
4. If a plugin owns it, put the subtree in the manifest (external) or
   `CommandSpec` (bundled) and a handler for each executable leaf.
5. Run `tests/completions_test.cpp` and the pty drawer test.

### Scenarios

- **CT-01**: loading `completions.json` makes every documented namespace and
  leaf completable, with help/man/choices/ranges.
- **CT-02**: a feed adds model/provider/policy/job/plugin leaves under
  documented namespaces; the rows are reachable and their actions dispatch.
- **CT-03**: a hostile subtree overwrites `set.model` action/help/man → the
  documented fields survive, its new child is added, `merge_collisions()`
  reports the attempts.
- **CT-04**: a dynamic node's state help refreshes between merges.
- **CT-05**: a plugin contribution mounts under `/plugin/<id>`; its attempted
  root/help/man/short writes are refused and counted, and its host-derived leaf
  action dispatches. A root is only the host's documented alias, never a plugin
  contribution.
- **CT-06**: `/settings`-style surfaces expose their children through the tree,
  not through a code-side list (see #289).

### Cross-references

- `tui/input-system/slash-engine.md` (drawer behaviour),
  `tui/input-system/auto-complete.md` (completion mechanics),
  `tui/input-system/contextual-help.md` (`?` pages),
  `plugins/developer-guide.md` (plugin-visible rule).

### Revision history

| Date | Reason |
|------|--------|
| 2026-10-20 | Mapper-only contract: documented fields immutable, children concatenated, collision counting, plugin roots must be new; code binds behaviour only (root rule superseded 2026-10-22) |
| 2026-10-22 | 0.6.0 placement/root policy: domains only, plugins mount under `/plugin/<id>`, settings before commands, `short` is an immutable shortcut field |
