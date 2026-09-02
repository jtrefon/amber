## Spec: Plugin Framework v2 — Harness Extension Engine

### Purpose

Make amber's plugin system the **extensibility backbone of the entire harness**.
Plugins don't just add agent tools — they enhance the platform in any direction:
UI rendering, LLM providers, memory backends, search engines, prompt templates,
compression strategies, and the agent loop itself.

**Demarcation with MCP:** MCP extends the *agent* (tools, resources, prompts
for the LLM). Plugins extend the *application* (UI, providers, memory, search,
events, themes, the agent loop itself). A plugin can observe and modify
anything the harness does; an MCP server can only offer capabilities to the
model. This is the core distinction: **plugin = app extension, MCP = agent
extension.**

This spec defines the v2 framework: a hybrid two-tier architecture where core
plugins run in-process for deep integration, and external plugins run as separate
processes for isolation. Both tiers implement the same `IPlugin` interface and
participate in a shared event bus.

The existing v1 plugin system (3-method JSON-RPC, tool-only) is preserved for
backward compatibility. V2 is additive — new plugins use the v2 interface;
existing plugins continue to work unchanged until a future migration.

### Context

The v1 plugin system (`include/agent/plugin.h`, `lib/plugin.cpp`) proved the
concept: external executables communicating via JSON-RPC over stdio, registering
tools in `ToolRegistry`, with a manifest-driven lifecycle. Two plugins shipped
(sysinfo, CDP). But v1 is limited to tool registration — plugins cannot observe
the agent loop, modify the TUI, register LLM providers, or contribute to the
command tree.

Meanwhile, several amber subsystems are candidates for plugin extraction:
LLM providers (currently hardcoded in `lib/providers_repo_files.cpp`), memory
backends (`lib/memory_store.cpp`), search backends (`tools/search/`), and the
MCP adapter itself (`lib/mcp_client.cpp`). A richer plugin framework would make
these swappable without recompiling amber.

### Ownership

- **Source files** (new): `include/agent/plugin_v2.h` (`IPlugin`, `Capability`,
  `PluginContext`), `include/agent/plugin_registry.h` (`PluginRegistry`),
  `include/agent/event_bus.h` (`EventBus`, `Event`, `EventType`),
  `lib/plugin_registry.cpp`, `lib/event_bus.cpp`
- **Plugin sources** (new): `plugins/theme/`, `plugins/prompt_interceptor/`,
  `plugins/google_llm/`
- **Test files** (new): `tests/plugin_v2_test.cpp`, `tests/event_bus_test.cpp`
- **Existing files unchanged**: `include/agent/plugin.h`, `lib/plugin.cpp`,
  `tools/plugins/` (v1 plugins continue as-is)
- **Spec status**: design — implementation tracked in this document.

---

### 1. Architecture: Hybrid Two-Tier

```
┌──────────────────────────────────────────────────────────────┐
│                       amber harness                           │
│                                                               │
│  ┌────────────────────────┐  ┌────────────────────────────┐  │
│  │   Core Plugins         │  │   External Plugins          │  │
│  │   (in-process)         │  │   (separate process)        │  │
│  │                        │  │                              │  │
│  │   IPlugin interface    │  │   IPlugin interface          │  │
│  │   + full capability    │  │   + JSON-RPC adapter         │  │
│  │                        │  │                              │  │
│  │   Proven by:           │  │   Proven by:                 │  │
│  │   - Google LLM provider│  │   - CDP (browser automation) │  │
│  │   - Theme engine       │  │   - SSH connector            │  │
│  │   - Prompt interceptor │  │   - Database connector       │  │
│  └────────────────────────┘  └────────────────────────────┘  │
│              │                            │                   │
│              └────────────┬───────────────┘                   │
│                           │                                   │
│               ┌───────────▼───────────┐                       │
│               │     PluginRegistry    │                       │
│               │  (lifecycle + event   │                       │
│               │   bus + capability    │                       │
│               │   registration)       │                       │
│               └───────────────────────┘                       │
└──────────────────────────────────────────────────────────────┘
```

**Core plugins** run in the same process as amber. They link against the core
library and have full access to harness internals via narrow interfaces. Used for
systems that need deep integration: LLM providers, memory backends, search
engines, TUI themes, prompt interceptors. A crash in a core plugin crashes the
harness (same as any other library code).

**External plugins** run as separate processes, communicating via JSON-RPC over
stdio (the v1 protocol, extended). Used for integrations that benefit from
isolation: browser automation, SSH, databases, custom tools. A crash in an
external plugin does not affect the harness.

Both tiers implement the same `IPlugin` interface. The `PluginRegistry` treats
them identically — it calls `initialize()`, reads `capabilities()`, and manages
lifecycle. The difference is internal: core plugins are direct C++ objects;
external plugins are wrapped in a `JsonRpcPluginAdapter` that bridges the
interface to the wire protocol.

### 2. IPlugin — The Plugin Interface

Every plugin, core or external, implements this interface:

```cpp
class IPlugin {
public:
    virtual ~IPlugin() = default;

    // Identity
    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
    virtual std::string name() const = 0;

    // Lifecycle
    virtual bool initialize(const PluginContext& ctx) = 0;
    virtual void shutdown() = 0;

    // Capability declaration
    virtual std::vector<Capability> capabilities() const = 0;
};
```

**Lifecycle states:**

```
Discovered → Registered → Active → Shutdown
                 ↓            ↓
              Failed      Deactivated
```

- **Discovered**: manifest found, plugin binary exists
- **Registered**: `PluginRegistry::register_plugin()` called
- **Active**: `initialize()` succeeded, capabilities registered
- **Failed**: `initialize()` returned false or threw
- **Deactivated**: `deactivate()` called, capabilities unregistered
- **Shutdown**: `shutdown()` called, resources released

### 3. Capability System

Plugins declare what they provide through the `Capability` type:

```cpp
struct Capability {
    enum class Type {
        Tool,           // registers tools in ToolRegistry
        Provider,       // registers an LLM provider
        Completion,     // contributes to the command tree
        Hook,           // observes/modifies agent events
        Theme,          // overrides TUI rendering
        PromptSource,   // provides prompt templates
        Memory,         // provides memory backend
        Search,         // provides search backend
    };

    Type type;
    std::string name;           // unique within the plugin
    std::string description;    // human-readable
    void* impl;                 // type-erased pointer to capability implementation
};
```

Each `Type` maps to a specific interface that `impl` points to:

| Type | `impl` points to | Registration target |
|------|------------------|-------------------|
| `Tool` | `ToolDef*` (name, description, schema, execute fn) | `ToolRegistry` |
| `Provider` | `ProviderImpl*` (chat, stream, list_models) | `ProviderRegistry` (new) |
| `Completion` | `CompletionNode*` (subtree for command tree) | `SettingRegistry` |
| `Hook` | `HookHandler*` (event type, callback) | `EventBus` |
| `Theme` | `ThemeImpl*` (color map, border style, layout overrides) | `TuiRenderer` |
| `PromptSource` | `PromptSourceImpl*` (list, get prompt templates) | `Agent` |
| `Memory` | `MemoryBackendImpl*` (store, search, list) | `MemoryStore` |
| `Search` | `SearchBackendImpl*` (search, index) | `ToolRegistry` (search tool) |

### 4. EventBus — Pub/Sub for Agent Lifecycle

The event bus is the backbone for plugin-harness communication. Plugins subscribe
to events. Events can be **observed** (read-only) or **intercepted** (can modify
or cancel).

#### Event Types

```cpp
enum class EventType {
    // Agent loop events
    AgentTurnStart,         // agent begins a turn (interceptable: modify prompt)
    AgentTurnEnd,           // agent completes a turn (observable)
    ToolCallBefore,         // before tool execution (interceptable: modify args, cancel)
    ToolCallAfter,          // after tool execution (observable: inspect result)
    MessageAdded,           // new message pushed to context (observable)
    CompressionTriggered,   // compression pipeline started (observable)

    // LLM events
    LLMRequestBefore,      // before HTTP request (interceptable: modify body)
    LLMTokenReceived,      // per-token streaming (observable: inspect/modify each token)
    LLMResponseAfter,      // after HTTP response (observable: inspect usage)

    // UI events
    TUIRender,             // before TUI render (interceptable: modify output)
    TUIKeyPress,           // key press (interceptable: consume or remap)
    TUIInputChanged,       // input buffer changed (observable)

    // Plugin lifecycle events
    PluginLoaded,          // a new plugin was loaded (observable)
    PluginUnloaded,        // a plugin was unloaded (observable)
};
```

#### Event Object

```cpp
struct Event {
    EventType type;
    void* data;              // type-specific payload (see below)
    bool cancelled = false;  // set by interceptors to skip default handling
};
```

Type-specific payloads:

| EventType | `data` type | Fields |
|-----------|------------|--------|
| `AgentTurnStart` | `TurnStartEvent*` | `std::string& prompt`, `Context& context` |
| `AgentTurnEnd` | `TurnEndEvent*` | `const Message& reply`, `TurnStats& stats` |
| `ToolCallBefore` | `ToolCallEvent*` | `const std::string& name`, `json& args`, `bool& cancel` |
| `ToolCallAfter` | `ToolResultEvent*` | `const std::string& name`, `ToolResult& result` |
| `MessageAdded` | `MessageEvent*` | `const Message& msg` |
| `CompressionTriggered` | `CompressionEvent*` | `size_t context_size`, `size_t threshold` |
| `LLMRequestBefore` | `LLMRequestEvent*` | `json& body`, `Config& cfg` |
| `LLMResponseAfter` | `LLMResponseEvent*` | `const json& response`, `int& status_code` |
| `TUIRender` | `RenderEvent*` | `RenderContext& ctx` (buffer, colors, layout) |
| `TUIKeyPress` | `KeyEvent*` | `int& key`, `bool& consumed` |
| `TUIInputChanged` | `InputEvent*` | `const std::string& buffer`, `size_t cursor` |

#### EventBus API

```cpp
class EventBus {
public:
    // Subscribe to an event (read-only observation)
    size_t subscribe(EventType type, std::function<void(const Event&)> handler);

    // Intercept an event (can modify data or set cancelled=true)
    size_t intercept(EventType type, std::function<bool(Event&)> handler);

    // Fire an event (interceptors first, then observers)
    // Returns false if any interceptor cancelled the event
    bool fire(EventType type, Event& event);

    // Unsubscribe
    void unsubscribe(size_t id);

    // Remove all handlers (called on shutdown)
    void clear();
};
```

**Execution order:** Interceptors run in reverse subscription order (last
registered runs first, like middleware). Observers run in subscription order.
If any interceptor sets `event.cancelled = true`, subsequent interceptors and
all observers are skipped, and `fire()` returns `false`.

### 5. PluginContext — What the Harness Provides

```cpp
struct PluginContext {
    EventBus& event_bus;              // subscribe/intercept events
    ToolRegistry& tools;              // register tools
    const Config& config;             // read-only config access
    const Workspace& workspace;       // workspace root

    // Future extensions (added as subsystems are plugin-ready):
    // ProviderRegistry& providers;
    // MemoryStore& memory;
    // SkillCatalog& skills;
};
```

The context is passed to `IPlugin::initialize()`. Plugins store a reference
and use it throughout their lifetime. The harness guarantees the context
remains valid for the plugin's entire lifecycle.

### 6. PluginRegistry — Lifecycle Management

```cpp
class PluginRegistry {
public:
    // Discovery (scans plugin directories, loads manifests)
    void discover();

    // Registration (core plugins call this directly)
    void register_plugin(std::shared_ptr<IPlugin> plugin);

    // Activation
    bool activate(const std::string& id);
    bool deactivate(const std::string& id);

    // Shutdown all
    void shutdown_all();

    // State
    enum class State { Discovered, Registered, Active, Failed, Deactivated, Shutdown };
    State state(const std::string& id) const;
    std::vector<PluginInfo> list() const;
    std::shared_ptr<IPlugin> find(const std::string& id) const;

    // Event bus (shared across all plugins)
    EventBus& event_bus();

    // Context (for passing to plugins during activation)
    void set_context(PluginContext ctx);
    const PluginContext& context() const;
};
```

**Discovery order** (same as v1):
1. `$XDG_CONFIG_HOME/amber/plugins/<id>/`
2. `~/.config/amber/plugins/<id>/`
3. `<workspace>/.amber/plugins/<id>/`
4. `$(datadir)/amber/plugins/<id>/`

**Core plugin registration** happens in `main()` before external discovery:

```cpp
PluginRegistry registry;
registry.set_context({event_bus, tools, config, workspace});

// Core plugins (in-process, registered directly)
registry.register_plugin(std::make_shared<GoogleLLMPlugin>());
registry.register_plugin(std::make_shared<ThemePlugin>());
registry.register_plugin(std::make_shared<PromptInterceptorPlugin>());

// External plugins (discovered from filesystem)
registry.discover();

// Activate all
for (auto& p : registry.list()) {
    if (p.state == State::Discovered)
        registry.activate(p.id);
}
```

### 7. Backward Compatibility with v1

The v1 plugin system (`include/agent/plugin.h`, `lib/plugin.cpp`) continues to
exist alongside v2. The relationship:

| v1 concept | v2 equivalent |
|-----------|--------------|
| `PluginManager` | `PluginRegistry` (v2) + `PluginManager` (v1, kept for external plugins) |
| `PluginTool` | Capability type `Tool` |
| `manifest.json` | Still used for external plugins; core plugins use code-only registration |
| `initialize` / `tool.call` / `shutdown` | Mapped to `IPlugin` lifecycle + `Tool` capability |
| `completion` in manifest | Capability type `Completion` |
| `/plugin` admin commands | Extended to show v2 plugin status |

**Migration path:** Existing v1 plugins (sysinfo, cdp) continue to work through
the `PluginManager` as before. A future phase will wrap v1 plugins in a
`V1PluginAdapter` that implements `IPlugin`, allowing them to participate in the
v2 event bus and capability system without rewriting their protocol.

### 8. External Plugin Protocol Extension

The v1 JSON-RPC protocol gains additional methods for v2 external plugins:

| Method | Direction | Purpose |
|--------|-----------|---------|
| `initialize` | harness → plugin | Handshake (unchanged) |
| `tool.call` | harness → plugin | Execute tool (unchanged) |
| `shutdown` | harness → plugin | Exit (unchanged) |
| `capabilities` | harness → plugin | **New:** query plugin capabilities |
| `hook.register` | harness → plugin | **New:** register event hooks |
| `hook.event` | harness → plugin | **New:** deliver event to plugin |
| `prompt.list` | harness → plugin | **New:** list prompt templates |
| `prompt.get` | harness → plugin | **New:** get a prompt template |
| `resource.list` | harness → plugin | **New:** list resources |
| `resource.read` | harness → plugin | **New:** read a resource |

V1 plugins that do not implement the new methods return a standard JSON-RPC
error (`Method not found`). The adapter falls back to v1 behavior.

### 9. Capability Interfaces

Each capability type has a narrow C++ interface that `impl` points to:

#### Tool Capability

```cpp
struct ToolDef {
    std::string name;
    std::string description;
    json schema;                            // JSON Schema for parameters
    std::function<ToolResult(const json&)> execute;
};
```

#### Provider Capability

```cpp
struct ProviderImpl {
    std::string name;                       // e.g. "google"
    std::vector<ModelInfo> models;          // available models
    std::function<ServerInfo(const Config&)> probe;
    std::function<Message(const Request&, const Config&)> chat;
    std::function<void(const Request&, const Config&, StreamCallback)> chat_stream;
};
```

#### Completion Capability

```cpp
struct CompletionNode {
    std::string action;                     // e.g. "plugin.theme.set"
    std::string help;                       // one-line drawer description
    std::string man;                        // manual page
    std::map<std::string, CompletionNode> children;
};
```

#### Hook Capability

```cpp
struct HookHandler {
    EventType event_type;
    bool intercept = false;                 // true = can modify/cancel
    std::function<bool(Event&)> handler;    // returns true to continue, false to cancel
};
```

#### Theme Capability

```cpp
struct ThemeImpl {
    std::string name;                       // e.g. "dracula"
    std::map<std::string, ColorPair> colors; // named color overrides
    std::function<void(RenderContext&)> apply; // render-time overrides
};
```

The `Theme` capability is the **floor**, not the ceiling. A theme plugin can
override colors, borders, and layout. A *UI plugin* (future `UI` capability)
can replace entire render passes, add new panels, inject widgets, or redesign
the TUI layout entirely. The `TUIRender` event is interceptable — a plugin can
replace the entire render output. The `TUIKeyPress` event is interceptable —
a plugin can consume or remap any key. Together these give a plugin the
ability to **redesign the entire UI experience**, not just re-skin it.

#### PromptSource Capability

```cpp
struct PromptSourceImpl {
    std::string namespace_;                 // e.g. "research"
    std::vector<PromptTemplate> templates;  // available templates
    std::function<std::string(const std::string& name, const json& args)> render;
};
```

#### Memory Capability

```cpp
struct MemoryBackendImpl {
    std::string name;                       // e.g. "sqlite", "vector"
    std::function<bool(const MemoryEntry&)> store;
    std::function<std::vector<MemoryEntry>(const std::string& query, size_t limit)> search;
    std::function<std::vector<MemoryEntry>()> list;
    std::function<bool(const std::string& id)> remove;
};
```

#### Search Capability

```cpp
struct SearchBackendImpl {
    std::string name;                       // e.g. "ripgrep", "semantic"
    std::function<std::vector<SearchResult>(
        const std::string& pattern,
        const std::string& path,
        const std::string& glob,
        size_t max_results)> search;
};
```

### 10. TUI Integration — Theme and UI Capture

Plugins can modify TUI rendering through the `Theme` capability and `TUIRender`
event interception.

#### Theme Plugin Example

```cpp
class ThemePlugin : public IPlugin {
public:
    std::string id() const override { return "theme"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Theme Engine"; }

    bool initialize(const PluginContext& ctx) override {
        ctx_ = &ctx;

        // Register theme capability
        static ThemeImpl dracula_theme{
            "dracula",
            {
                {"bg", ColorPair(COLOR_BLACK, COLOR_MAGENTA)},
                {"fg", ColorPair(COLOR_WHITE, COLOR_BLACK)},
                {"accent", ColorPair(COLOR_CYAN, COLOR_BLACK)},
                {"error", ColorPair(COLOR_RED, COLOR_BLACK)},
            },
            [](RenderContext& ctx) {
                // Override status bar rendering
                ctx.status_bar_style = StatusBarStyle::Compact;
            }
        };

        Capability cap;
        cap.type = Capability::Type::Theme;
        cap.name = "dracula";
        cap.impl = &dracula_theme;
        capabilities_.push_back(cap);

        // Intercept TUI render events
        ctx.event_bus.intercept(EventType::TUIRender, [this](Event& e) {
            auto* data = static_cast<RenderEvent*>(e.data);
            active_theme_->apply(data->ctx);
            return true; // continue processing
        });

        return true;
    }

    // ...
};
```

#### Prompt Interceptor Plugin Example

```cpp
class PromptInterceptorPlugin : public IPlugin {
public:
    bool initialize(const PluginContext& ctx) override {
        ctx_ = &ctx;

        // Register hook capability
        static HookHandler handler{
            EventType::AgentTurnStart,
            true,  // intercept
            [this](Event& e) -> bool {
                auto* data = static_cast<TurnStartEvent*>(e.data);
                // Inject project context into the prompt
                data->prompt = enhance_prompt(data->prompt);
                return true;
            }
        };

        Capability cap;
        cap.type = Capability::Type::Hook;
        cap.name = "prompt_enhancer";
        cap.impl = &handler;
        capabilities_.push_back(cap);

        return true;
    }

    // ...
};
```

### 11. Security Model

| Property | Core plugins | External plugins |
|----------|-------------|-----------------|
| **Trust level** | Trusted (in-process) | Untrusted (separate process) |
| **Crash impact** | Harness crash | No impact (process isolated) |
| **Path confinement** | Bypassed (full access) | Enforced via `Workspace::confine` |
| **Approval gate** | Not required (trusted) | Required for dangerous tools |
| **Output cap** | None (trusted) | 64 KiB (same as v1) |
| **Event interception** | Full read/write | Read-only (interceptors cannot cancel) |

External plugins follow the same security model as v1: untrusted code running
with the user's privileges, path-confined, output-capped. The `trusted` flag
from MCP can be applied to external plugins that are explicitly approved.

Core plugins run in-process and are implicitly trusted. They have full access
to the harness. Users install core plugins by adding them to the build or
loading them via `dlopen()` — both require explicit user action.

### 12. Proof-of-Concept Plugins

The v2 framework is proven by building new plugins, NOT by migrating existing
code. Each PoC validates a specific capability type.

| PoC | Type | Capabilities proven | Files |
|-----|------|-------------------|-------|
| **Theme Engine** | Core | `Theme`, `Hook` (TUI interception) | `plugins/theme/` |
| **Prompt Interceptor** | Core | `Hook` (agent loop interception) | `plugins/prompt_interceptor/` |
| **Google LLM** | Core | `Provider` (LLM provider registration) | `plugins/google_llm/` |
| **CDP v2** | External | `Tool`, `Completion` (backward compat) | `tools/plugins/cdp/` (update) |

#### PoC 1: Theme Engine

Proves: TUI event interception, theme capability, color override system.

- Registers a `Theme` capability with a color map
- Intercepts `TUIRender` events to apply theme colors
- Provides 2 built-in themes: "amber" (default) and "dracula"
- `/plugin theme set dracula` activates a theme
- Demonstrates that plugins can modify rendering without touching TUI code

#### PoC 2: Prompt Interceptor

Proves: Agent loop observation, prompt modification, context injection.

- Registers a `Hook` capability on `AgentTurnStart`
- Intercepts the prompt before LLM call
- Injects project-specific context (git status, open files, recent errors)
- `/plugin prompt_interceptor set context=git` enables git context injection
- Demonstrates that plugins can modify agent behavior without touching the agent loop

#### PoC 3: Google LLM Provider

Proves: Provider registration, model discovery, chat/streaming.

- Registers a `Provider` capability
- Implements Google Gemini API (REST, libcurl)
- Provides model listing, chat, and streaming
- `/plugin google_llm set api_key=...` configures the provider
- `/provider add google` makes Gemini models available
- Demonstrates that new LLM providers can be added without recompiling amber

#### PoC 4: CDP v2 Update

Proves: External plugin backward compatibility, capability declaration.

- Updates the existing CDP plugin to declare `Tool` and `Completion` capabilities
- Does NOT change the wire protocol (still v1 JSON-RPC)
- The `V1PluginAdapter` wraps the existing plugin in the `IPlugin` interface
- Demonstrates that v1 plugins work with the v2 registry

### 13. Implementation Phases

| Phase | What | Depends on | Proves |
|-------|------|-----------|--------|
| **1. Framework** | `IPlugin`, `Capability`, `EventBus`, `PluginRegistry`, `PluginContext` | Nothing | Core abstractions compile and pass unit tests |
| **2. Integration** | Wire `PluginRegistry` into `Agent`, `TUI`, `main.cpp` | Phase 1 | Lifecycle management works end-to-end |
| **3. PoC: Theme** | Theme plugin (core) | Phase 2 | TUI interception works |
| **4. PoC: Prompt** | Prompt interceptor plugin (core) | Phase 2 | Agent loop hooks work |
| **5. PoC: Provider** | Google LLM plugin (core) | Phase 2 | Provider registration works |
| **6. PoC: CDP** | Update CDP to new interface | Phase 2 | External plugins work |
| **7. Documentation** | Plugin developer guide, API reference | Phase 6 | Others can build plugins |

**Phase 1 is the current scope.** Phases 2-7 are planned but not started.

### 14. File Structure

```
include/agent/
  plugin_v2.h              ← IPlugin, Capability, PluginContext, capability interfaces
  plugin_registry.h        ← PluginRegistry
  event_bus.h              ← EventBus, Event, EventType

lib/
  plugin_registry.cpp      ← PluginRegistry implementation
  event_bus.cpp            ← EventBus implementation

plugins/                   ← new directory for core plugin sources
  theme/
    theme_plugin.h
    theme_plugin.cpp
  prompt_interceptor/
    prompt_plugin.h
    prompt_plugin.cpp
  google_llm/
    google_plugin.h
    google_plugin.cpp

tests/
  plugin_v2_test.cpp       ← IPlugin, Capability, PluginRegistry unit tests
  event_bus_test.cpp       ← EventBus unit tests (subscribe, intercept, fire, cancel)

docs/spec/plugins/
  README.md                ← v1 spec (unchanged)
  plugin-framework-v2.md   ← this document
  developer-guide.md       ← how to write amber plugins
```

### 15. Conventions

Follow all conventions from `AGENTS.md`:
- **TDD**: write failing test first, implement, verify green
- **Size limits**: class ≤200 lines, method ≤10 lines with minimal branching
- **SOLID**: each capability type is a separate interface (ISP), plugins depend
  on abstractions (DIP), new capability types extend without modifying the registry (OCP)
- **RAII**: plugin lifecycle managed by `shared_ptr`, no raw ownership
- **Error handling**: `IPlugin::initialize()` returns `bool`, never throws.
  Capability methods may throw; the registry catches and marks the plugin as failed.
- **No comments that restate code**: document intent, not syntax
- **Clang-format**: LLVM-based, 4-space indent, no tabs, 100 cols

### 16. Open Questions (deferred to implementation)

1. **`dlopen()` for core plugins**: should core plugins be loadable at runtime
   via shared libraries, or compiled into the binary? The spec allows both;
   implementation will decide based on the first PoC.

2. **Provider registry**: the current `lib/providers_repo_files.cpp` is
   file-based. A `ProviderRegistry` abstraction is needed for v2. Design deferred
   to Phase 5 (Google LLM PoC).

3. **Memory backend interface**: the current `MemoryStore` is file-based. A
   `MemoryBackendImpl` interface is defined in this spec but not implemented
   until a PoC needs it.

4. **Plugin dependencies**: can one plugin depend on another? Not in v1. Deferred
   to a future phase.

5. **Plugin hot-reload**: can a plugin be updated without restarting amber? Not
   in v1 or v2 initial. Deferred.
