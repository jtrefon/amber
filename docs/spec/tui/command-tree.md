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
- Patching an existing namespace from a plugin; a plugin owns a new root.
- A second mapper file or a code-generated tree that bypasses the loader.

---

### Dynamic namespaces

Runtime content (models, providers, policy rules, jobs, plugins, MCP servers)
is injected as **completions.json-shaped JSON subtrees**:

```cpp
nlohmann::json subtree = nlohmann::json::object();
subtree["set"]["children"]["provider"]["children"][name]["action"] = action;
subtree["set"]["children"]["provider"]["children"][name]["help"] = "switch to this provider";
register_action(action, handler);
settings.merge_completions_json(subtree);
```

A feed registers only the leaf action closures; it never builds a completion
row list in C++.

#### Merge semantics

`merge_completions_json()` is additive and collision-checked:

1. **Documented fields are immutable.** A node path that `completions.json`
   supplied may not have `action`, `help`, `man`, `aliases`, `choices` or
   `range` rewritten. The documented value wins and the attempt increments
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
  lists.
- The host derives every leaf action as `plugin.<id>.<path>`, so a plugin
  cannot name (or overwrite) a core action.
- A plugin root must be **new**. `refresh_completions()` and
  `install_plugin_commands()` check `has_top_level_command()` first; a
  contribution whose root already exists (documented, another plugin, or a
  feed) is skipped whole and reported on the debug/status channel. Plugins
  extend the surface with a namespace; they never patch a core one.
- Handlers are code, but they bind only the leaves their subtree declares. A
  leaf with no handler is documented and inert.

---

### Adding a command

1. Add the node to `completions.json` (nested under `children`), with `action`,
   `help`, and `man`; add `choices`/`range`/`aliases` where they apply.
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
- **CT-05**: a plugin root that already exists is skipped; a new root is
  merged and its host-derived actions dispatch.
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
| 2026-10-20 | Mapper-only contract: documented fields immutable, children concatenated, collision counting, plugin roots must be new; code binds behaviour only |
