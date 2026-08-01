# Amber Plugin Framework — Specification

**Status:** Implemented (v1)
**Owner:** amber core
**Version:** protocol 1

Plugins are self-contained programs that extend amber with new agent tools and
new slash-command namespaces. The harness never links plugin code: every plugin
is a separate executable speaking JSON-RPC over stdio, so a plugin crash cannot
take down the app and protocol changes ship as plugin updates without touching
the core.

## 1. Model

```
┌─────────────────────┐   spawn/pipe    ┌──────────────────────────┐
│ amber (harness)     │ ◄──────────────► │ plugin executable        │
│  PluginManager      │   JSON-RPC 2.0  │  e.g. cdp-plugin          │
│  ToolRegistry       │   newline-deli. │  (WebSocket → Chrome)     │
└─────────────────────┘                 └──────────────────────────┘
```

- **Discovery.** A plugin is a directory containing `manifest.json` plus the
  executable. Search order (first hit wins): `$XDG_CONFIG_HOME/amber/plugins/<id>`,
  `~/.config/amber/plugins/<id>`, `<workspace>/.amber/plugins/<id>`,
  `$(datadir)/amber/plugins/<id>` (system-shipped).
- **Lifecycle.** `available` (manifest valid, found) → `enabled` (tools
  registered, namespaces merged) → `disabled`; `incompatible` when the
  `protocol_version` handshake fails. Enabled plugins are spawned on first tool
  call and kept alive until disabled or the harness exits.
- **I/O contract.** Everything a plugin exchanges with the harness is JSON.
  Inputs: JSON-RPC requests. Outputs: JSON-RPC responses whose `result` is the
  tool envelope `{ok, output, meta}` — the same contract as built-in tools, so
  the agent reads plugin results exactly like `bash` or `search` results.
- **Isolation.** The plugin runs unprivileged like the harness. Path arguments
  are confined to the workspace by the harness before dispatch. Output is
  capped (64 KiB) by the harness.

## 2. Manifest (`manifest.json`)

```json
{
  "id": "cdp",
  "name": "CDP Browser",
  "version": "1.2.0",
  "protocol_version": 1,
  "author": "…",
  "url": "https://…",
  "license": "MIT",
  "description": "How and when to use this plugin — advertised to the agent "
                 "in the tool prompt when the plugin is enabled.",
  "main": "cdp-plugin",
  "settings": { "endpoint": "ws://127.0.0.1:9222" },
  "completion": {
    "cdp": {
      "action": "plugin.cdp",
      "help": "one-line drawer description",
      "man": "Manual page: use case, usage, examples.",
      "children": {
        "navigate": { "action": "plugin.cdp.navigate",
                      "help": "…", "man": "…" }
      }
    }
  },
  "tools": [
    { "name": "navigate", "description": "…",
      "schema": { "url": {"type": "string", "required": true} } }
  ]
}
```

| Field | Required | Meaning |
|-------|----------|---------|
| `id` | yes | Slug, `[a-z0-9_]+`, used for the directory name, tool prefix and namespace root. |
| `name` | yes | Display name. |
| `version` | yes | Semver, e.g. `1.2.0`. Used for package management. |
| `protocol_version` | yes | Integer. Harness rejects plugins with a different value (`incompatible`). |
| `author` / `url` / `license` | no | Attribution metadata. |
| `description` | yes | Prompt advertisement: how/when to use the plugin's tools. Rendered into the agent's system prompt while enabled. |
| `main` | yes | Executable path relative to the plugin directory. |
| `settings` | no | Default key/value settings; overridden via `/plugin set <id> <key>=<value>` and persisted next to the manifest. |
| `completion` | no | A completions.json subtree (same node shape: `action`, `help`, `man`, `children`, `choices`, `range`). Merged into the command tree on enable. The `action` paths use the `plugin.<id>…` namespace. |
| `tools` | no | Tool definitions. Each: `name` (registered as `plugin_<id>_<name>`), `description` (agent-facing), `schema` (JSON Schema object for `parameters`). |

## 3. Protocol (JSON-RPC 2.0, newline-delimited, UTF-8)

One JSON object per line on stdin/stdout. Requests from harness, responses from
plugin. All messages carry `id` (except notifications).

### 3.1 initialize

```json
{"id": 1, "method": "initialize", "params": {
  "protocol_version": 1,
  "settings": { "endpoint": "ws://127.0.0.1:9222" },
  "workspace": "/path/to/workspace"}}
```

Response: `{"id": 1, "result": {"protocol_version": 1, "ok": true}}`.
A `protocol_version` mismatch, or `"ok": false`, marks the plugin
`incompatible`/failed and unregisters it.

### 3.2 tool.call

```json
{"id": 2, "method": "tool.call",
 "params": {"name": "navigate", "args": {"url": "https://example.com"}}}
```

Response — the tool envelope:

```json
{"id": 2, "result": {"ok": true, "output": "https://example.com — title", "meta": {}}}
```

Errors are returned, never thrown: `{"ok": false, "output": "ERROR: …"}`.

### 3.3 shutdown (notification)

```json
{"method": "shutdown"}
```

The plugin exits cleanly. The harness also SIGKILLs stragglers.

### 3.4 Timeouts

A tool call that produces no line within 60 s is killed and reported as a
timeout (partial output lost — plugins must emit one line per request).

## 4. Tool registration and the agent

- Tools register in `ToolRegistry` as `plugin_<id>_<name>` (security prefix:
  collides with nothing, greppable, clearly not a core tool).
- Enabled plugin tools appear in the agent's tool prompt via a generated
  "Plugins" section appended to the system prompt (each: name, description,
  schema summary).
- Namespaces merge into `SettingRegistry` via the same walker that loads
  `completions.json`; the drawer and `?` help then work for plugin commands
  with zero extra code.

## 5. Administration

- `/plugin list | status <id> | enable <id> | disable <id> | get <id> [key] |
  set <id> <key>=<value> | info <id> | install <path|url> | uninstall <id>`
- **install**: tar.gz archive (local path or http(s) URL via libcurl),
  containing `manifest.json` + executable. Validated (id, protocol version,
  main exists, executable bit) → staged to `~/.config/amber/plugins/<id>/` →
  enabled.
- **uninstall**: disables and deletes the user-level plugin directory.
  System-shipped plugins cannot be uninstalled (read-only).

## 6. Security model

- Plugin code is **untrusted** but runs with the user's privileges, like the
  harness itself. Operators install only plugins they trust; `/plugin info`
  shows author/url/license before enabling.
- All path arguments are passed through `Workspace::confine` before the plugin
  sees them; the plugin cannot escape the workspace through amber.
- Output is capped (64 KiB) per tool result.
- The `plugin.<id>.*` action namespace and `plugin_<id>_*` tool prefix keep
  plugin capabilities visibly namespaced; approval rules can target them by
  name like any other tool.

## 7. Bundled plugins

| Plugin | Ships in | Purpose |
|--------|----------|---------|
| `sysinfo` | core package | Host facts for the agent: memory/swap, CPU load, partitions, network interfaces and addresses. |
| `cdp` | separate package | Real browser automation via the Chrome DevTools Protocol (WebSocket to a CDP endpoint): list targets, navigate, evaluate JS, click, type, screenshot to file, DOM snapshot. |
