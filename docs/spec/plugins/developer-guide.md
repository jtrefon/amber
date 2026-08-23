# Amber Plugin Developer Guide

This guide explains how to build plugins for amber. It covers both core plugins
(in-process, for deep integration) and external plugins (separate process, for
isolation).

## Quick Start

### External Plugin (Simplest)

An external plugin is a standalone executable that speaks JSON-RPC over stdio.
It needs only three things: a manifest, a main loop, and the ability to handle
`initialize`, `tool.call`, and `shutdown` messages.

**1. Create the plugin directory:**

```
~/.config/amber/plugins/hello/
├── manifest.json
└── hello-plugin
```

**2. Write `manifest.json`:**

```json
{
  "id": "hello",
  "name": "Hello Plugin",
  "version": "1.0.0",
  "protocol_version": 1,
  "author": "Your Name",
  "url": "https://github.com/you/amber-hello",
  "license": "MIT",
  "description": "A minimal example plugin. Adds a /hello command.",
  "main": "hello-plugin",
  "tools": [
    {
      "name": "greet",
      "description": "Greets the user by name",
      "schema": {
        "name": {
          "type": "string",
          "required": true,
          "description": "Name to greet"
        }
      }
    }
  ],
  "completion": {
    "hello": {
      "action": "plugin.hello",
      "help": "Hello plugin commands",
      "man": "A simple greeting plugin.",
      "children": {
        "greet": {
          "action": "plugin.hello.greet",
          "help": "Greet someone by name",
          "man": "Usage: /hello greet <name>"
        }
      }
    }
  }
}
```

**3. Write the plugin (Python example):**

```python
#!/usr/bin/env python3
"""Minimal amber plugin — reads JSON-RPC from stdin, writes to stdout."""
import sys, json

def handle(msg):
    method = msg.get("method")
    if method == "initialize":
        return {"protocol_version": 1, "ok": True}
    elif method == "tool.call":
        name = msg["params"]["name"]
        args = msg["params"]["args"]
        if name == "greet":
            return {"ok": True, "output": f"Hello, {args.get('name', 'world')}!"}
        return {"ok": False, "output": f"Unknown tool: {name}"}
    elif method == "shutdown":
        sys.exit(0)
    return {"ok": False, "output": f"Unknown method: {method}"}

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    msg = json.loads(line)
    resp = {"id": msg.get("id"), "result": handle(msg)}
    print(json.dumps(resp), flush=True)
```

**4. Make it executable:**

```bash
chmod +x ~/.config/amber/plugins/hello/hello-plugin
```

**5. Enable it in amber:**

```
/plugin enable hello
```

The plugin's `greet` tool is now available to the agent, and `/hello greet
<name>` appears in the command tree.

---

## Architecture Overview

```
amber harness                    plugin executable
┌──────────────────┐            ┌──────────────────┐
│ PluginRegistry   │◄── pipe ──►│ main loop        │
│   ├─ discover()  │  JSON-RPC  │   read stdin     │
│   ├─ activate()  │            │   handle method  │
│   └─ deactivate()│            │   write stdout   │
│ ToolRegistry     │            └──────────────────┘
│ EventBus         │
│ SettingRegistry  │
└──────────────────┘
```

- **Core plugins** are C++ classes linked into amber. They implement `IPlugin`
  directly and have full access to the harness.
- **External plugins** are separate processes. They communicate via newline-delimited
  JSON-RPC 2.0 over stdin/stdout. A `V1PluginAdapter` in amber bridges the
  wire protocol to the `IPlugin` interface.

Both types register capabilities (tools, completions, hooks, etc.) through the
same `PluginRegistry` API.

---

## Plugin Lifecycle

```
Discovered → Registered → Active → Shutdown
                 ↓            ↓
              Failed      Deactivated
```

1. **Discovered**: amber finds `manifest.json` in a plugin directory.
2. **Registered**: `PluginRegistry::register_plugin()` is called.
3. **Active**: `initialize()` succeeds. Capabilities are registered in the
   harness (tools in `ToolRegistry`, completions in `SettingRegistry`, etc.).
4. **Failed**: `initialize()` returned false or threw. The plugin is skipped.
5. **Deactivated**: `deactivate()` was called. Capabilities are unregistered.
6. **Shutdown**: `shutdown()` was called. Resources are released.

### Lifecycle Methods

| Method | When called | What to do |
|--------|------------|------------|
| `initialize(ctx)` | Once, at activation | Store context, register capabilities, subscribe to events |
| `capabilities()` | After `initialize` | Return the list of capabilities this plugin provides |
| `shutdown()` | Once, at deactivation | Release resources, unsubscribe from events |

---

## Capability Types

### Tool

Register a tool that the agent can call:

```cpp
ToolDef tool;
tool.name = "greet";
tool.description = "Greets the user by name";
tool.schema = json{{"name", {{"type", "string"}, {"required", true}}}};
tool.execute = [](const json& args) -> ToolResult {
    std::string name = args.value("name", "world");
    return {true, "Hello, " + name + "!", "", json{}};
};

Capability cap;
cap.type = Capability::Type::Tool;
cap.name = "greet";
cap.impl = &tool;
```

### Completion

Contribute a subtree to the command tree:

```cpp
CompletionNode node;
node.action = "plugin.hello";
node.help = "Hello plugin commands";
node.man = "A simple greeting plugin.";
node.children["greet"] = CompletionNode{
    "plugin.hello.greet",
    "Greet someone by name",
    "Usage: /hello greet <name>",
    {}
};

Capability cap;
cap.type = Capability::Type::Completion;
cap.name = "hello";
cap.impl = &node;
```

### Hook

Observe or intercept agent events:

```cpp
HookHandler hook;
hook.event_type = EventType::AgentTurnStart;
hook.intercept = true;  // can modify the event
hook.handler = [](Event& e) -> bool {
    auto* data = static_cast<TurnStartEvent*>(e.data);
    data->prompt = "You are a helpful assistant.\n" + data->prompt;
    return true;  // continue processing
};

Capability cap;
cap.type = Capability::Type::Hook;
cap.name = "prompt_enhancer";
cap.impl = &hook;
```

### Theme

Override TUI rendering:

```cpp
static ThemeImpl my_theme{
    "my_theme",
    {
        {"bg", ColorPair(COLOR_BLACK, COLOR_BLUE)},
        {"fg", ColorPair(COLOR_WHITE, COLOR_BLACK)},
    },
    [](RenderContext& ctx) {
        ctx.status_bar_style = StatusBarStyle::Compact;
    }
};

Capability cap;
cap.type = Capability::Type::Theme;
cap.name = "my_theme";
cap.impl = &my_theme;
```

### Provider

Register an LLM provider:

```cpp
static ProviderImpl my_provider;
my_provider.name = "my_llm";
my_provider.models = {{"my-model-1", 4096}, {"my-model-2", 8192}};
my_provider.probe = [](const Config& cfg) -> ServerInfo {
    // Check if the server is reachable
    return {true, "my-model-1", 4096};
};
my_provider.chat = [](const Request& req, const Config& cfg) -> Message {
    // Make API call and return response
    return Message{"assistant", "Hello!"};
};

Capability cap;
cap.type = Capability::Type::Provider;
cap.name = "my_llm";
cap.impl = &my_provider;
```

---

## Event Bus

Subscribe to events to observe or modify agent behavior:

```cpp
// Observe (read-only)
ctx.event_bus.subscribe(EventType::ToolCallAfter, [](const Event& e) {
    auto* data = static_cast<ToolResultEvent*>(e.data);
    std::cerr << "Tool " << data->name << " returned ok=" << data->result.ok << "\n";
});

// Intercept (can modify or cancel)
ctx.event_bus.intercept(EventType::ToolCallBefore, [](Event& e) -> bool {
    auto* data = static_cast<ToolCallEvent*>(e.data);
    if (data->name == "dangerous_tool") {
        data->cancel = true;  // prevent execution
        return false;
    }
    return true;  // continue
});
```

### Event Types Reference

| Event | Interceptable | `data` type | Use case |
|-------|:------------:|------------|----------|
| `AgentTurnStart` | Yes | `TurnStartEvent*` | Modify prompt, inject context |
| `AgentTurnEnd` | No | `TurnEndEvent*` | Log stats, update UI |
| `ToolCallBefore` | Yes | `ToolCallEvent*` | Modify args, block execution |
| `ToolCallAfter` | No | `ToolResultEvent*` | Log results, update state |
| `MessageAdded` | No | `MessageEvent*` | Track conversation |
| `CompressionTriggered` | No | `CompressionEvent*` | Monitor compression |
| `LLMRequestBefore` | Yes | `LLMRequestEvent*` | Modify request body |
| `LLMResponseAfter` | No | `LLMResponseEvent*` | Inspect usage |
| `TUIRender` | Yes | `RenderEvent*` | Override rendering |
| `TUIKeyPress` | Yes | `KeyEvent*` | Remap keys |
| `TUIInputChanged` | No | `InputEvent*` | Track input |
| `PluginLoaded` | No | `PluginEvent*` | React to new plugins |
| `PluginUnloaded` | No | `PluginEvent*` | Cleanup |

---

## Manifest Reference

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique slug: `[a-z0-9_]+`. Used for directory name, tool prefix, namespace. |
| `name` | string | Display name. |
| `version` | string | Semver (e.g. `1.2.0`). |
| `protocol_version` | integer | Must match harness (`1` for v1). |
| `description` | string | How/when to use this plugin. Rendered in system prompt. |
| `main` | string | Executable path relative to plugin directory. |

### Optional Fields

| Field | Type | Description |
|-------|------|-------------|
| `author` | string | Attribution. |
| `url` | string | Homepage / source repository. |
| `license` | string | SPDX license identifier. |
| `settings` | object | Default key/value settings. Overridable via `/plugin set`. |
| `tools` | array | Tool definitions (name, description, schema). |
| `completion` | object | Command tree subtree (action, help, man, children). |

---

## Installation

### User plugins

Place in `~/.config/amber/plugins/<id>/`:

```bash
mkdir -p ~/.config/amber/plugins/myplugin
cp manifest.json myplugin-plugin ~/.config/amber/plugins/myplugin/
chmod +x ~/.config/amber/plugins/myplugin/myplugin-plugin
```

### Workspace plugins

Place in `<workspace>/.amber/plugins/<id>/`:

```bash
mkdir -p .amber/plugins/myplugin
cp manifest.json myplugin-plugin .amber/plugins/myplugin/
chmod +x .amber/plugins/myplugin/myplugin-plugin
```

### System plugins

Place in `$(datadir)/amber/plugins/<id>/` (requires root):

```bash
sudo cp -r myplugin /usr/share/amber/plugins/
```

### Install from archive

```
/plugin install /path/to/myplugin-1.0.0.tar.gz
/plugin install https://example.com/myplugin-1.0.0.tar.gz
```

The archive must contain `manifest.json` and the executable at its root.

---

## Admin Commands

| Command | Description |
|---------|-------------|
| `/plugin list` | List all discovered plugins and their state |
| `/plugin status <id>` | Show detailed status of a plugin |
| `/plugin enable <id>` | Activate a plugin (initialize + register capabilities) |
| `/plugin disable <id>` | Deactivate a plugin (unregister + shutdown) |
| `/plugin info <id>` | Show manifest metadata (author, url, license) |
| `/plugin get <id> [key]` | Read a plugin setting |
| `/plugin set <id> <key>=<value>` | Set a plugin setting |
| `/plugin install <path\|url>` | Install from tar.gz archive |
| `/plugin uninstall <id>` | Remove a user-installed plugin |

---

## Security

- External plugin code is **untrusted**. It runs with your privileges but is
  isolated in a separate process. A crash does not affect amber.
- Path arguments are confined to the workspace before the plugin sees them.
- Output is capped at 64 KiB per tool call.
- Plugin tools appear in the agent's tool list with the `plugin_<id>_` prefix.
  Approval rules can target them by name.
- Core plugins are **trusted** (in-process). They have full access to the
  harness. Only install core plugins you trust.

---

## Differences: Core vs External

| Property | Core plugin | External plugin |
|----------|------------|----------------|
| Language | C++ | Any (Python, Go, Rust, etc.) |
| Process | Same as amber | Separate process |
| Communication | Direct C++ calls | JSON-RPC over stdio |
| Crash impact | Harness crash | No impact |
| Path confinement | Bypassed | Enforced |
| Performance | Zero overhead | IPC overhead |
| Capability access | All types | All types (via protocol extension) |
| Hot-reload | No | No (restart required) |

---

## Examples

See the proof-of-concept plugins in `plugins/`:
- `plugins/theme/` — TUI theme override (core plugin)
- `plugins/prompt_interceptor/` — agent prompt modification (core plugin)
- `plugins/google_llm/` — LLM provider registration (core plugin)

See the existing v1 plugins for external plugin patterns:
- `tools/plugins/sysinfo/` — host telemetry (C++)
- `tools/plugins/cdp/` — browser automation (C++, WebSocket)

See the test fixture for a minimal example:
- `tests/plugins/fake_plugin.py` — Python echo plugin (23 lines)
