# Plugin API compatibility

**Status:** 1.0 policy. This document is the compatibility promise a plugin
author can rely on.

## Core (bundled) plugins

Core plugins compile against the installed headers in `include/agent/` and link
the runtime. The compiler is the binary compatibility check; the source
compatibility promise is:

- **Stable within 1.x:** the documented `IPlugin` methods, `Capability` and its
  concrete capability classes, `PluginServices`, `PluginContext`,
  `PromptRegistry`, `StatusRegistry`, `PanelRegistry`, `CommandRegistry`,
  `KeybindingRegistry`, `SettingsRegistry`, `SearchBackendRegistry`,
  `WalletRegistry`, and the event payloads in `events.h`.
- **Additive changes are allowed:** a new `CapabilityKind`, a new optional
  virtual with a default, a new registry method, a new event type, or a new
  field with a default. Existing plugins keep compiling.
- **Breaking changes are staged:** a removal or a signature change requires a
  deprecation release in the 1.x line and a documented migration. A change that
  cannot be staged waits for 2.0.
- **Not stable:** the internals of `Agent`, the compression pipeline, the TUI
  and the built-in tool implementations. A host embedding the library should
  hold `IAgent` and the documented ports, not reach into `Agent`'s internals.
  At 1.0 the C++ embedding API is explicitly **not** frozen; that is tracked in
  the post-1.0 decomposition issue.

## Plugin identity and state

- A plugin id is a stable identifier. Renaming one is a breaking change and
  requires a state migration.
- Bundled plugin state lives at
  `$XDG_CONFIG_HOME/amber/plugins/<id>/plugin.conf` (enabled flag) and at the
  plugin's own settings path. External plugin settings live in
  `plugins/state.json`. The exact store is documented per capability.
- Plugin ids are lowercase; bundled ids may contain `-`, external ids must match
  `[a-z0-9_]+` until the external manifest grammar is widened.

## External (process) plugins

- **Protocol version 1** is the wire contract. A manifest with a different
  `protocol_version` is rejected as incompatible.
- The external tier is **tools-only** at 1.0. Tools are registered as
  `plugin_<id>_<name>` through the owner-scoped port; manifest `completion`
  subtrees are not mounted.
- A response line is capped at 64 KiB. A plugin that exceeds it fails the call.
- The process starts lazily on the first tool call and is shut down on disable.
- `kPluginApiVersion` is a **runtime** gate for a future out-of-tree binary
  tier. It does not exist at 1.0 because the process tier negotiates
  `protocol_version` and the bundled tier is compiler-checked. If a future
  release loads code that was not built with the harness, that release adds the
  constant and its rejection path together.

## What is not promised

- ABI stability for compiled-in plugins across different compiler/standard
  library versions.
- The internal layout of `PluginRuntime`, its private ledger, or the TUI.
- Third-party plugin code running with fewer privileges than the harness user.
- Hot reload, plugin dependency resolution, or loading plugin code at runtime
  (PF-6, deferred).
