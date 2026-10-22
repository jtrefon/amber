# Spec: Slash command namespace and plugin placement

- **Status:** Build contract for 0.6.0. Decisions locked 2026-10-22.
- **Tracking:** #319; sub-issues #322–#325; release #331.
- **Related:** `command-tree.md`, `command-shortcuts.md`, `plugins/developer-guide.md`,
  `plugins/plugin-framework.md`, `tui/input-system/slash-engine.md`.

## 1. Problem

Before 0.6.0 the command tree had grown by adding a root for each feature: 19
core roots plus plugin roots, including action roots (`new`, `close`, `save`,
`stop`, `compress`, `sessions`). `CommandSpec` let a plugin choose another root,
and `command-tree.md` explicitly permitted it. The result was predictable: a
drawer with too many one-word peers, prefix collisions, and no place to add
agents, projects, and work surfaces without more chaos.

The completion drawer is the primary discovery surface. A root namespace is a
scarce resource, not a deployment convenience.

## 2. The grammar

Every command is:

```text
<domain> <verb> [<id>] [args...]
```

The only roots are domains. Features are children of a domain; actions are verbs,
not roots.

### 2.1 Domains

| Domain | Owns | Canonical verbs |
|---|---|---|
| `session` | conversations | `list`, `new`, `load`, `save`, `remove`, `fork`, `rename`, `compress` |
| `window` | screens and attachment | `list`, `new`, `close`, `switch`, `rename`, `attach`, `detach` |
| `job` | background processes | `list`, `start`, `show`, `read`, `stop` |
| `agent` | background model workers (subagents) | `list`, `show`, `stop` |
| `watch` | long-running command/PR/URL polls (plugin) | `list`, `show`, `report`, `stop` |
| `project` | project roots and project sessions | `list`, `open`, `show`, `close` |
| `worktree` | git worktrees | `list`, `new`, `attach`, `detach`, `remove`, `show`, `prune` |
| `work` | cross-resource board | `list`, `show` |
| `plugin` | extension workspace | `list`, `info`, `<plugin-id> ...` |
| `get` / `set` | read-only properties and configuration | domain keys only |
| `files` / `system` | tool domains | their existing verbs |
| `provider` / `settings` / `panel` | existing configuration/view domains | unchanged |
| `help` / `quit` | meta | unchanged |

`agent` is the short root; help text says “background subagents.” The main agent
is the session, not a managed resource.

`watch` is listed as a domain because the status bar and Work board use that
vocabulary. In 0.6.0 its commands live under `/plugin watch`; `/watch` is only a
host-owned alias if the allowlist grants one. There is no plugin root by default.

### 2.2 Verbs

| Verb | Meaning |
|---|---|
| `list` | enumerate a collection |
| `show` | one resource's details/status |
| `new` | create a local resource |
| `open` | create or focus a resource/surface |
| `attach` | bind a resource to a viewing surface |
| `detach` | unbind a viewing surface; the resource continues |
| `start` | launch asynchronous work |
| `stop` | terminate asynchronous work (job/watch/agent/turn) |
| `close` | close a surface/session; not kill a resource |
| `save` | persist a conversation/session |
| `remove` | remove a saved object or registry entry |
| `rename` | change a human label |
| `read` | read a resource's output |
| `report` | read a bounded watch report |
| `fork` | duplicate a conversation |
| `prune` | maintenance of registry/artifacts |

Use `stop` for jobs and watches. `kill` and `cancel` are compatibility aliases.
Use `remove` for saved sessions/worktrees; `delete` is a compatibility alias.
Use `show` for detail; `status` is a compatibility alias.

## 3. Read, write, and action planes

- `/get` is **read-only** and never mutates, starts, stops, attaches, or closes.
  It exposes properties and read-only catalogues.
- `/set` is configuration only. It never creates or manages runtime resources.
- Resource roots own resource lifecycle.
- `/window` owns attachment and screen lifecycle.
- `/work` is a **view**: it lists and shows across kinds. It is not a second
  lifecycle API.

Examples:

```text
/get project path
/get session title
/get subagent active
/get subagent max
/set subagent parallel
/get job count
/job list
/agent list
/work list
/work show job 3
/window attach job 3
/window detach
/job stop 3
```

`/get job` and `/get subagent` are not management aliases. The canonical
instance lists are `/job list` and `/agent list`.

## 4. Plugin placement: settings first, workspace second, root last

A plugin chooses a **contribution kind**, not a namespace root.

### 4.1 Settings and state

A plugin setting is a typed property, not a command root. The target capability:

```cpp
struct PluginSetting {
    std::string key;
    std::string help;
    std::string type;       // string | bool | int | choice | path
    std::string value;      // current value
    std::vector<std::string> choices;
    std::function<bool(const std::string&)> set;
};
```

The host generates:

```text
/get plugin settings <id> <key>
/set plugin settings <id> <key> <value>
```

and a group in `/settings`. The plugin supplies schema and behavior; the host
owns shape, completion, help and persistence. Bundled plugins use the same
capability as external plugins. A bespoke plugin config file is not a command
surface.

### 4.2 Actions: plugin workspace

Actions mount under:

```text
/plugin <id> <verb> [args]
```

The host composes the mount from the plugin id. The plugin's `CommandSpec`
subtree is **relative to that workspace**; it does not name a root. External
manifest `completion` subtrees are mounted the same way. Every leaf action stays
host-derived as `plugin.<id>.<path>`.

### 4.3 Root: host-owned allowlist

A root is a scarce, host-owned UI decision. A plugin may request one, but cannot
register it. The host allowlist is part of the command tree, not the plugin
manifest. Root aliases are documented in `completions.json` and may be removed
without changing the plugin.

**0.6.0 default: no plugin roots.** Existing `watch`, `mcp`, `prompt`, `index`
and `hello` commands move under `/plugin <id>`. If a first-class daily-use
domain proves it needs a root alias later, the host grants it explicitly.

### 4.4 Placement decision tree

1. Is it a setting or read-only state? → `SettingsCapability` / `/get`/`/set`.
2. Is it an action? → `/plugin <id>`.
3. Is it a tool? → tool capability; no command surface.
4. Is it a panel/status/prompt? → that capability; no command root.
5. Is it a core, every-day domain? → host root allowlist (not plugin code).

## 5. Migration table

Canonical paths replace action roots. Legacy names remain **hidden aliases for
one release**, then are removed.

| Legacy | Canonical |
|---|---|
| `/new`, `/clear`, `/reset` | `/session new` |
| `/close` | `/window close` |
| `/save` | `/session save` |
| `/stop`, `/cancel`, `/kill` | `/session stop` (active turn) |
| `/compress`, `/compact` | `/session compress` |
| `/sessions`, `/load`, `/open` | `/session list` |
| `/session delete` | `/session remove` |
| `/job kill` | `/job stop` |
| `/watch cancel` | `/watch stop` (or `/plugin watch stop` after plugin migration) |
| `/worktree status` | `/worktree show` |
| `/panel` | `/panel` (view domain; keep) |
| `/watch`, `/mcp`, `/prompt`, `/index`, `/hello` | `/plugin <id>` |

A compatibility alias never appears as a documented peer in the drawer; it is
accepted, with a one-line notice when appropriate, and removed after the
migration window.

## 6. Framework and metadata changes

- `CommandSpec.root` is removed from the plugin-facing API; the host assigns the
  `/plugin/<id>` mount. Existing plugin roots are a migration concern, not a
  future option.
- `PluginManifest.completion` is a subtree under the plugin workspace.
- Add `SettingsCapability` for typed plugin settings; `/get plugin settings` and
  `/set plugin settings` are generated by the host.
- The command-tree merge continues to be additive and collision-checked.
  Documented fields (`action`, `help`, `man`, `aliases`, `short`, `choices`,
  `range`) are immutable. Plugin contributions cannot rewrite a documented
  field or name a core action.
- Root aliases, if granted, are documented in `completions.json` by the host.
- Plugin protocol bumps because command mount semantics change.

## 7. Acceptance tests

- **NS-01** every documented root is a domain; no feature/action roots.
- **NS-02** `/get` cannot dispatch a mutating action; `/set` cannot manage a
  resource.
- **NS-03** `/plugin <id>` mounts a plugin subtree; the plugin cannot add a root.
- **NS-04** a plugin settings schema generates `/get plugin settings` and
  `/set plugin settings` entries, help, choices and persistence.
- **NS-05** legacy aliases dispatch to canonical commands; they are absent from
  the documented drawer.
- **NS-06** a merge attempt to rewrite a documented root/field is refused and
  counted.
- **NS-07** the command tree and settings screen are generated from the same
  JSON; no code-side command list.
- **NS-08** the Work board and resource windows use the same domain vocabulary
  (`session`, `job`, `agent`, `watch`) as the commands.

## 8. Non-goals

- Not a compatibility layer that keeps both a plugin root and a workspace as
  permanent surfaces.
- Not a second command tree in code.
- Not a generic fuzzy search engine; shortcuts and unique prefixes are the
  deliberate fast path.
- Not a reason to move tool-only plugins into the command tree.
