# Amber Plugin Framework, Specification

**Status:** Implemented
**Owner:** amber core
**Version:** external plugin protocol 1

> **Scope note.** This document specifies the **external** plugin tier only:
> separate processes, JSON-RPC over stdio, tools as the sole capability. The
> in-process **core plugin** framework (providers, prompt blocks, status
> segments, panels, wallets, typed events, ledger-managed
> enable/disable) is a separate contract: `plugin-framework.md`. The two tiers
> share the word "plugin" and nothing else.

Plugins are self-contained programs that extend amber with new agent tools. The
harness never links plugin code: every plugin is a separate executable speaking
JSON-RPC over stdio, so a plugin crash cannot take down the app and protocol
changes ship as plugin updates without touching the core. The external tier is
tools-only at 1.0; command namespaces are a bundled-plugin capability.

## 1. Model

```
┌─────────────────────┐   spawn/pipe    ┌──────────────────────────┐
│ amber (harness)     │ ◄──────────────► │ plugin executable        │
│  PluginManager      │   JSON-RPC 2.0  │  e.g. cdp-plugin          │
│  ToolRegistry       │   newline-deli. │  (WebSocket → Chrome)     │
└─────────────────────┘                 └──────────────────────────┘
```

- **Discovery.** A plugin is a directory containing `manifest.json` plus the
  executable. A directory without a manifest is not a plugin — the v2 runtime
  keeps its per-plugin state (`plugin.conf`) under the same root — so it is
  skipped rather than reported as incompatible. Search order (first hit wins):
  `$XDG_CONFIG_HOME/amber/plugins/<id>`,
  `~/.config/amber/plugins/<id>`, `<workspace>/.amber/plugins/<id>`,
  `$(datadir)/amber/plugins/<id>` (system-shipped).
- **Lifecycle.** `available` (manifest valid, found) → `enabled` (tools
  registered through the owner-scoped port) → `disabled`; `incompatible` when
  the manifest is invalid. Enabled plugins are spawned on first tool call and
  kept alive until disabled or the harness exits, so activation never blocks
  the host. Disabling cancels an in-flight call at its next poll slice and
  waits at most one second for a graceful exit before SIGKILL, so a hung
  plugin cannot freeze a plugin toggle.
- **I/O contract.** Everything a plugin exchanges with the harness is JSON.
  Inputs: JSON-RPC requests. Outputs: JSON-RPC responses whose `result` is the
  tool envelope `{ok, output, meta}`, the same contract as built-in tools, so
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
  "description": "How and when to use this plugin, advertised to the agent "
                 "in the tool prompt when the plugin is enabled.",
  "main": "cdp-plugin",
  "settings": { "endpoint": "ws://127.0.0.1:9222" },
  "tools": [
    { "name": "navigate", "description": "…",
      "schema": { "url": {"type": "string", "required": true} } }
  ]
}
```

| Field | Required | Meaning |
|-------|----------|---------|
| `id` | yes | Slug, `[a-z0-9_]+`, used for the directory name and tool prefix. |
| `name` | yes | Display name. |
| `version` | yes | Semver, e.g. `1.2.0`. Used for package management. |
| `protocol_version` | yes | Integer. Harness rejects plugins with a different value (`incompatible`) on the first handshake. |
| `author` / `url` / `license` | no | Attribution metadata. |
| `description` | yes | Prompt advertisement: how/when to use the plugin's tools. Rendered into the agent's system prompt while enabled. |
| `main` | yes | Executable path relative to the plugin directory. |
| `settings` | no | Default key/value settings; exposed through `/get plugin settings <id> <key>` and `/set plugin settings <id> <key> <value>`, persisted next to the manifest. |
| `completion` | no | **Ignored at 1.0.** The external tier is tools-only; a manifest's command subtree is not mounted. |
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

Response, the tool envelope:

```json
{"id": 2, "result": {"ok": true, "output": "https://example.com, title", "meta": {}}}
```

Errors are returned, never thrown: `{"ok": false, "output": "ERROR: …"}`.

### 3.3 shutdown (notification)

```json
{"method": "shutdown"}
```

The plugin exits cleanly. Teardown cancels an in-flight call first and waits
at most one second for the exit, then SIGKILLs a straggler; the host never
waits out a tool call's 60 s deadline.

### 3.4 Timeouts

A tool call that produces no line within 60 s is killed and reported as a
timeout (partial output lost, plugins must emit one line per request).

## 4. Tool registration and the agent

- Tools register in `ToolRegistry` as `plugin_<id>_<name>` through the runtime's
  owner-scoped port. Disabling the plugin removes exactly its set, even under a
  name collision.
- Enabled plugin tools appear in the agent's tool prompt via a generated
  "Plugins" section appended to the system prompt (each: name, description,
  schema summary).
- Command namespaces are not an external capability at 1.0. A bundled
  `CommandCapability` is the supported path.

## 5. Administration

- `/get plugin list | info <id> | settings <id> [key]`
- `/set plugin on <id> | off <id> | install <path|url> | uninstall <id> |
  settings <id> <key>=<value>`
- **install**: tar.gz archive (local path or http(s) URL via libcurl),
  containing `manifest.json` + executable. Validated (id, protocol version,
  main exists, executable bit) → staged to `~/.config/amber/plugins/<id>/` and
  left **disabled** — install and run are separate steps, so `/set plugin on
  <id>` activates it deliberately.
- **uninstall**: disables and deletes the user-level plugin directory.
  System-shipped plugins cannot be uninstalled (read-only).

## 6. Security model

- Plugin code is **untrusted** but runs with the user's privileges, like the
  harness itself. Operators install only plugins they trust; `/get plugin info
  <id>` shows author/url/license before enabling.
- All path arguments are passed through `Workspace::confine` before the plugin
  sees them; the plugin cannot escape the workspace through amber.
- Output is capped (64 KiB) per response line; a plugin that exceeds it or
  emits no newline fails the call. Framing and reply decoding are pure
  functions in `include/agent/plugin_protocol.h` (no file descriptors), so the
  unit suite and `fuzz/plugin_protocol.cpp` drive the same code the manager
  reads bytes into.
- Plugin stderr is discarded (the protocol owns stdout), so a chatty plugin
  cannot paint over the TUI.
- External plugin tools are owner-tagged. The `plugin_<id>_` prefix keeps them
  visibly namespaced; approval rules can target them by name like any tool.
- `state.json` and per-plugin `plugin.conf` are owner-only and written
  atomically.

## 7. Shipped external plugins

| Plugin | Ships in | Purpose |
|--------|----------|---------|
| `sysinfo` | core package | Host facts for the agent: memory/swap, CPU load, partitions, network interfaces and addresses. |
| `cdp` | core package | Real browser automation via the Chrome DevTools Protocol (WebSocket to a CDP endpoint): list targets, navigate, evaluate JS, click, type, screenshot to file, DOM snapshot. The Linux `.deb`/`.rpm` also ship a standalone `amber-plugin-cdp` package. |
