## Spec: Plugin Framework, Harness Extension Engine

- **Status:** Implemented through PF-4 (PF-1..PF-4 complete, 2026-09-10). This
  document is the **design record**: the contract the framework was built to.
  The **authority on what is available today is the availability table in
  `docs/spec/plugins/developer-guide.md`**; where this document describes a
  surface that is not implemented, the guide and the code win. Code and this
  spec disagree → the code is wrong.
- **Supersedes:** the aspirational sections of the earlier framework draft
  (which described `void*` capabilities, an untyped event bus, and capability
  routing that no code performed). The external-plugin protocol
  (`plugins/README.md`) is a separate, unchanged, still-shipping contract.
- **Related:** `docs/spec/llm-client/dialect.md` (the provider wire seam this
  framework's first real capability plugs into), `docs/spec/tui/layout-engine.md`,
  `docs/spec/context/context-ownership-and-parallel-compression.md`,
  `docs/spec/plugins/developer-guide.md` (author-facing contract).

### Purpose

Make amber extensible in place: providers, tools, commands, prompt blocks, status
segments, panels, settings, and log sinks are **contributed by plugins through
typed registries**, and **observed through typed events**: with correct
enable/disable, no core edits per extension, and no ambiguity about which thread
a callback runs on.

The framework exists to make *adding an LLM provider* a plugin-shaped task, but
it is deliberately general: the same registries that carry a provider carry a
status readout, a prompt block, or a console panel. The first three consumers are
amber itself (metrics, the registry console, and the provider set), so the
framework is dogfooded before it is opened to contributors.

---

### 1. Why the framework was rebuilt (historical, 2026-09-10)

The previous framework draft defined a plugin system that does not run. Verified state
before this design (2026-09-10):

| Claim in the old draft | Reality in the tree |
|---|---|
| Plugins register capabilities routed by the registry | `capabilities()` is called only by tests; no routing exists (`tests/plugin_core_test.cpp`) |
| `PluginContext` lets a plugin add tools | `tools` is `const ToolRegistry&` (`plugin_core.h:37`); `register_tool` is non-const (`registry.h:29`), the Tool capability is unreachable |
| The EventBus carries agent lifecycle | `fire()` is called only by tests and the dormant metrics plugin; **zero production fire sites** (`lib/event_bus.cpp:22`) |
| Plugins can render UI / intercept keys (`TUIRender`, `TUIKeyPress`) | No TUI source fires or consumes them; keys are a hardcoded `switch` (`tui/tui.cpp:557-607`) |
| Plugin command namespaces work | external plugin subtrees merge into the tree but **no `plugin.*` action is ever registered** (`tui/tui_input.cpp:1253-1261`), so the drawer entries cannot execute |
| Core plugins are activated at startup | `tui/tui_main.cpp:158` constructs a `PluginRegistry`; nothing ever calls `register_plugin()` or `activate()` outside tests |

What *does* work and is preserved: the external-plugin tier (subprocess
JSON-RPC, tools only, `lib/plugin.cpp`), the `PluginRegistry` lifecycle state
machine, `EventBus` semantics (snapshot-under-lock, LIFO interceptors,
re-entrancy, pinned by `tests/event_bus_test.cpp`), and `AgentHooks` as the
host UI channel.

The rebuild is justified because five separate needs (provider plugins, UI
contributions, prompt/context extension, error observability, an extensible
status bar) all resolve to the same three mechanisms. Building them once, typed
and ledger-owned, is cheaper and safer than five bespoke seams.

---

### 2. The extension model

Three mechanisms, with one rule that keeps them from collapsing into each other:

| Mechanism | Purpose | Direction | Examples |
|---|---|---|---|
| **Contribution registries** | Things that *exist* while a plugin is active | Plugin → harness, at activation, host-arbitrated | tools, providers, commands, prompt blocks, status segments, panels, settings, log sinks |
| **Events** | Things that *happen* | Harness → plugin, at the moment they happen | turn started/ended, tool requested/completed, message added, compression, LLM response, errors |
| **Host services** | Things the plugin needs *the host to do* | Plugin → host, on demand | `ask_secret`, `choose`, `confirm`, `notify`, `post_to_ui`, `log` |

**Rule: registration never happens through events.** Events are notifications;
contributions are explicit installs that produce a removable handle. If
registration were event-driven, ordering, timing, and deactivation would become
unspecifiable.

**Rule: UI state is pulled, not pushed.** Panels and status segments are
callables the host invokes at render time on the UI thread. Plugins never touch
ncurses and never draw into a frame directly.

#### Where plugin code runs (tiers)

Where a plugin executes is an isolation decision, not a size decision. A large
codebase is not a reason for a process; untrusted code, a foreign language, and
crash containment are. The framework defines three tiers and adopts two:

| Tier | Mechanism | Adopted | Crash domain | Language | Cost per call |
|---|---|---|---|---|---|
| **Bundled (core)** | compiled into amber, `plugins/<id>/` | **Yes, PF-1..PF-4** | shared with harness | C++17 | none (direct call) |
| **External (process)** | separate process, framed IPC | Deferred to PF-6; shaped for | isolated | any | IPC round trip |
| **Loadable library** | `dlopen` a shared object | **Rejected** (D16) | shared with harness | C++ (ABI-locked) | none |

**Bundled is the only tier that ships now**, and it is the only tier that can
carry amber's own providers: the streaming decoder is invoked per SSE chunk, so
the built-in providers must never cross an address space. Disabling a bundled
plugin costs nothing at runtime, registration happens at startup, dialect
factories are constructed lazily on first use, and a disabled plugin has no
subscriptions and no registry rows (invariant 9).

**The external tier gets a different capability shape, not the same one.** A
process cannot use the in-process `Dialect` port: the transport, the config, and
the decoder all live in the harness address space. So an external provider owns
its **own** HTTP client and credentials, receives the conversation over IPC, and
streams chunks back, the same relationship an MCP server has to a tool. That is
a genuine second shape, and it duplicates retry, timeout, cancellation, and usage
accounting in every such plugin; acceptable for third-party code, unacceptable
for ours. The framework therefore keeps the process tier *out of the provider
path for bundled providers* and reserves it for code we do not ship.

**Consequences designed in now, built later:** capabilities stay declarative
(data first, callables second) so a wire form is expressible later; state layout
is already per-plugin (`§9`); `kPluginApiVersion` exists before any external
consumer; and no plugin may touch harness internals, so nothing has to be
un-picked when the boundary moves.

**IPC choices, when PF-6 opens:** reuse the shapes already in the tree, the external
plugin protocol (newline-delimited JSON-RPC over stdio, `lib/plugin.cpp`) and the
MCP transport (`lib/mcp_transport.cpp`), rather than inventing a third. A unix
socket is preferred over stdio pipes for a provider (bidirectional streaming with
explicit framing and cancellation). Per-token IPC is affordable at human token
rates; framing, cancellation propagation, and backpressure are the real costs,
and they are why this is not built until the in-process port is proven.

**Threads are not a tier.** A thread shares the address space and the crash
domain, so it provides parallelism, never isolation. Amber already runs the agent
loop and compression on their own workers; plugins use threads only as an
internal implementation detail.

---

### 3. Capabilities and the ledger

A capability is a typed object that knows how to install itself:

```cpp
class Capability {
public:
    virtual ~Capability() = default;
    virtual std::string name() const = 0;        // unique within the plugin
    virtual CapabilityKind kind() const = 0;     // Tool, Provider, Command, ...
    virtual InstallResult install(PluginServices&) = 0;   // returns a handle
};
```

`IPlugin::capabilities()` returns `std::vector<std::unique_ptr<Capability>>`
(core tier) and stays the single declarative statement of what a plugin
provides, it feeds the console, the ledger, and the enable/disable path.

**The ledger is the contract.** Every accepted install returns a `Contribution`
handle; the runtime records it against the plugin id. `disable(id)`:

1. runs `shutdown()` on the plugin,
2. unwinds the ledger for that id in **reverse install order** (removing tools,
   commands, segments, panels, subscriptions, provider rows),
3. marks the plugin `Deactivated`.

If any contribution survives deactivation, the ledger is broken, and that is a
tested invariant, not an aspiration (`plugin_disable_removes_every_contribution`).

**Rejected alternatives:**

- *`void* impl` + `Type` enum (the old draft).* Untyped: every consumer
  re-invents the cast contract, a wrong cast is UB, and the API cannot be
  published to contributors. Typed polymorphic capabilities cost one virtual
  call at install time and nothing at run time.
- *Imperative registration by the plugin (`ctx.tools().add(...)` called from
  `initialize`).* Deactivation correctness becomes the plugin author's problem;
  one forgotten unsubscribe leaks a callback into freed state. The registry
  installs; the plugin declares.
- *One monolithic `PluginApi` object with 20 methods.* Violates ISP and makes
  the console's "what did this plugin contribute" query impossible to answer
  generically. Each registry is a narrow interface; a common
  `ExtensionPoint` view (`kind()`, `items()`, `remove(owner)`) gives the console
  one listing loop.

---

### 4. Extension points

Each registry is a narrow interface (`ISP`) and implements the common
`ExtensionPoint` introspection view. Ordering is explicit wherever output order
is observable.

| Registry | Contribution | Ordering | Notes |
|---|---|---|---|
| **Tools** | `Tool` instances (`unique_ptr`) | registry order | The existing `ToolRegistry` (`include/agent/registry.h`) grows an owner-tagged `add`/`remove`; the const-ref bug in `PluginContext` is fixed by exposing a narrow `ToolSink` |
| **Providers** | `ProviderCapability` = flavor + optional dialect factory + preset rows | registration order | Registers into the dialect table (`lib/dialect.cpp:31`) and the provider repository list; see §7 |
| **Prompt blocks** | `PromptBlock{id, priority, render(snapshot) -> std::string}` | ascending priority | Core memory/skills/brief blocks migrate onto this registry (see §5) |
| **Status segments** | `StatusSegment{id, priority, drop_priority, text, tone}` against a host-published `StatusSnapshot` | ascending priority; `drop_priority` decides overflow | Composed by `StatusRegistry`; amber's own segments register the same way (`lib/core_segments.cpp`), so a plugin's segment is indistinguishable from a core one. Rendering stays a pure read, time-driven work goes in `IPlugin::tick()` |
| **Panels** | `PanelSpec{id, title, lines(width), on_key}` | registration order | Host owns framing, scrolling, cycling and key routing; a focused panel gets first refusal on keys. The registry console (Alt+0/`/panel`) is core UI built on this API |
| **Commands** | `CommandSpec{root, help, man, subtree, handlers}` | registration order | The host merges the subtree into the command tree and binds each executable leaf's derived action (`plugin.<id>.<path>`) to its handler. Handlers return the text to display, so the plugin stays UI-free. TUI-only: the headless CLI has no command tree |
| **Wallets** | `WalletRegistry::Fetch = optional<WalletSnapshot>(const Config&)` | keyed by provider id | One mechanism for one question, "what is left?", because a prepaid balance and a set of quota windows are the same idea in two shapes, and two registries for it cost two flags, two status segments and two command pairs that drift. A provider supplies only the *fetch*. The runtime owns when to refresh (turn end, provider switch, startup), the cache, and the rendering, so every provider's readout behaves identically and no plugin carries a poll loop or a cache. One core status segment: the balance when the snapshot has one, otherwise the window closest to reset; `-` when the provider declares none or the fetch failed. `/get provider wallet` carries the whole picture; `/set provider wallet on\|off` is the display switch |

**Plugin registry state is itself a command-tree surface** (`plugin` namespace
under `/get` and `/set`, D17/D18):

| Command | Behaviour |
|---|---|
| `/get plugin list` | Every registered plugin: id, tier, state (on/off), and what it contributes |
| `/get plugin info <id>` | One plugin's detail: version, api version, capabilities, contributions, state source — plus the manifest for external plugins |
| `/get plugin settings <id> [key]` | An external plugin's persisted settings |
| `/set plugin on <id>` \| `off <id>` | The single write path for enable/disable; persists to the plugin's `plugin.conf` and applies immediately |
| `/set plugin install <path\|url>` | Stage an archive into the user plugin directory, left disabled |
| `/set plugin uninstall <id>` | Deactivate and delete an installed plugin |
| `/set plugin settings <id> <key>=<value>` | Per-plugin settings, stored next to the state |

The ids are **feed values under their verb** (`get.plugin.info.<id>`,
`set.plugin.on.<id>`), never siblings of the commands, so the drawer of
`/get plugin` or `/set plugin` stays a command list however many plugins
register. The root `/plugin` namespace is retired (D18): packaging verbs moved
under `/set plugin`, so there is one surface where there used to be two.

**Toggling re-publishes the surfaces the plugin fed.** Enabling or disabling
refreshes the provider feed (`refresh_provider_feed`) and the command tree, so
`/get provider list` and the drawer reflect the change immediately, a disabled
provider plugin's rows are gone, an enabled one's appear, without a restart.

**Commands (re-added).** A `CommandCapability` contributes a slash-command
namespace: a root, its help/man, the child nodes (completions.json shape), and
one handler per executable leaf. The host merges the subtree and binds each
leaf's derived action (`plugin.<id>.<path>`) to its handler. Handlers return the
text to display, so a command plugin stays UI-free. The bundled
`plugins/hello/` is the worked example (`/hello greet <name>`). The TUI owns the
slash engine, so a command contribution is TUI-only today.

**Removed extension points.** **Per-plugin settings** was cut before landing,
because nothing produced and nothing consumed it; the `CapabilityKind` enum does
not carry it and no plugin may register one. It reopens when a real consumer
exists.

**Deferred extension points** (not in this framework version, with reasons):
themes, raw key interception, window geometry/z-order, per-token stream events,
log sinks, hot reload, and loading plugin code at runtime. See §10.

---

### 5. Prompt blocks (and the chat_once refactor)

Today `Agent::chat_once` injects four kinds of block with four different ad-hoc
insertion algorithms: learned memories (`agent.cpp:244-263`), skill discovery and
activated skill bodies (`:290-319`), and the session brief (`:327-335`), after
`ensure_system_prompt` assembles the base prompt (`:190-204`).

The framework replaces that with one ordered registry:

```
base system prompt (prompts/system.md, tools, env card, MCP)
  + prompt block: learned memory        (priority 100)
  + prompt block: skill discovery       (priority 200)
  + prompt block: session brief         (priority 300)
  + prompt block: <plugin contributions>
  + activated skill bodies              (tail)
  + conversation
```

`Agent` asks the registry for blocks in priority order and appends each
non-empty block as its own `system` message on the **prompt copy**: never on the
sealed `Context` (invariant preserved from the existing design).

**KV-prefix rule:** block order and content must be deterministic for a given
turn. A block whose content changes every turn invalidates the server's prefix
cache from its insertion point onward; that is acceptable only at the tail
(which is why the brief is last). Plugin authors must document their block's
volatility. This is the one performance-relevant contract of the registry.

The core blocks migrating onto the registry is deliberate dogfooding: if the
registry cannot express amber's own prompt assembly, it cannot express a
plugin's.

**Status of that migration (2026-09-11): resolved, and the answer was not the
one the design assumed.** Reading the history showed the pre-compression memory
block was a **regression**, not a layout preference: before the immutable-Context
rewrite, compression ran on a list that already contained the injection, and the
rewrite's `prompt_copy.assign(...)` started discarding it. The fix is one
assembly point *after* the gate (`Agent::inject_prompt_blocks`), with the
head/tail placement preserved so ordinary turns are byte-identical.

The core blocks were **deliberately not moved into `PromptRegistry`**: the
registry is runtime-owned and shared across windows, while the retriever, skill
catalog and brief store are per-agent, so a core block registered there would put
one window's state into another window's prompt. The registry is for app-wide
contributions; core blocks are assembled by the agent that owns their state, and
both are merged in one ordered pass (`prompt_priority`). Full contract:
`docs/spec/agent-loop/prompt-assembly.md`.

---

### 6. Events

Events are typed. The existing untyped `EventBus` remains as the primitive
(it is tested and its semantics are pinned); a typed layer sits above it:

```cpp
// include/agent/events.h, payloads, no void* in the plugin-facing API
struct TurnStartedEvent  { std::string prompt; };
struct TurnEndedEvent    { const Message& reply; };
struct MessageAddedEvent { const Message& msg; };
struct ToolRequestedEvent{ std::string name; json args; bool cancel = false; };
struct ToolCompletedEvent{ std::string name; const ToolResult& result; long duration_ms; };
struct LlmResponseEvent  { long status; long prompt_tokens; long completion_tokens; };
struct CompressionEvent  { long tokens; long budget; double threshold; };
struct ErrorRaisedEvent  { std::string kind; std::string message; bool retryable; };

// typed over the existing bus
template <class E> Subscription subscribe(std::function<void(const E&)>);
template <class E> void publish(const E&);
```

Catalogue (fires on the **owner** thread unless stated; interceptable = payload
may be mutated or the event cancelled):

| Event | Fire site | Interceptable | Notes |
|---|---|---|---|
| `TurnStarted` | `Agent::run` after the user prompt is pushed | yes (prompt text) | The last word before the first LLM request |
| `TurnEnded` | `finish_turn` / `finish_turn_cancelled` | no | |
| `MessageAdded` | every `Context::push` site in the agent loop and dispatch | no | Tool-result pushes are included (a gap in the old `ContextEventSource`) |
| `ToolRequested` | `dispatch.cpp:191` before the approval gate | yes (args, cancel) | Fires before approval: "the model asked", not "this will run" |
| `ToolCompleted` | `dispatch.cpp:266-284` | no | |
| `LlmResponseReceived` | `chat_once` after `chat`/`chat_stream` returns | no | Hidden `confirm_turn` exchanges are **not** published (they use `silent_hooks`; same gate applies) |
| `CompressionTriggered` / `CompressionCompleted` | gate decision `agent.cpp:266-278`; rebuild `:458-461` | no | Structured replacement for the current `on_status` prose |
| `ErrorRaised` | retry/classification paths `agent_helpers.cpp:134-249`, auth repair `agent.cpp:699-719` | no | Observation only: **error classification stays in the dialect** (`is_retryable`, `context_overflow_hint`) |
| `PluginLoaded` / `PluginUnloaded` | - | no | Declared in `events.h`, **never published** (no fire site). Kept for a future consumer; do not subscribe expecting them. |

**Performance invariants (hard):**

1. `publish<E>()` with no subscribers for `E` performs a single relaxed atomic
   load and returns, no allocation, no virtual dispatch, no lock.
2. Nothing publishes per token. Token streaming remains an `AgentHooks` concern
   for the host UI; a plugin needing stream progress subscribes to a coalesced
   event added deliberately, not to a per-token fire.
3. `publish` never allocates in the unsubscribed path; subscribers are copied
   out under the lock only when present (existing `fire()` behaviour).
4. Events are not a general-purpose notification bus for the UI: `AgentHooks`
   stays the host UI channel. Publishing must not duplicate hook traffic.

**Rejected alternatives:**

- *Replacing `AgentHooks` with the bus.* Hooks are a request/response contract
  (approval, API-key prompt) whose callbacks block until the host answers; that
  is not an event. Both stay, with distinct purposes.
- *`void*` payloads (old draft).* Requires every subscriber to know an
  out-of-band cast contract; a type error is UB. The typed layer costs one
  trait mapping per event.
- *Firing events from the host by translating `AgentHooks`.* Cheap, but
  compression lifecycle, error classification, and request/response are not
  hooks, the translation would fabricate events from prose (`on_status`
  strings) and produce a bus that lies.

---

### 7. Provider plugins (the first real consumer)

A provider plugin contributes a `ProviderCapability`. (An earlier draft called
this `ProviderSpec` with an `AuthSpec`; neither type exists in the tree, the
delivered shape below is what the code uses.)

```cpp
// include/agent/extensions.h, the delivered type.
class ProviderCapability : public Capability {
public:
    struct Preset {
        std::string name;          // provider name, e.g. "gemini"
        std::string api_base;
        std::string default_model;
        bool requires_key = true;
    };

    // `make_dialect` empty: presets only, speaking a protocol provided
    // elsewhere (the shared openai dialect).
    ProviderCapability(std::string flavor,
                       std::function<std::unique_ptr<class Dialect>()> make_dialect = {},
                       std::vector<Preset> presets = {});
    ...
};
```

- `make_dialect` registers into the **existing** dialect table
  (`register_dialect`, `include/agent/dialect.h:89`), the port is already
  public and already documented as the extension point.
- `presets` become provider rows through the existing repository merge
  (`ProviderService::available()`), so they appear in `/provider list` and the
  command feed with no new concept.
- The dialect supplies endpoints, auth headers, body, buffered parse, stream
  decoding, model listing, usage mapping, and error classification, everything
  wire-specific. **No boolean capability flags**: behavior lives in the dialect,
  the plugin supplies data and structure.

**What a provider plugin must not do:** reimplement transport, own an HTTP
client, or bypass the dialect. `lib/http_transport.cpp` stays the single curl
mechanism for every provider.

**First consumer: Gemini.** Chosen because it is genuinely incompatible
(query-parameter key, `generateContent` endpoints, `contents`/`parts` body,
`usageMetadata`, different streaming shape) and therefore exercises every
dimension of the port. It proves the framework end to end: dialect + presets +
commands + settings + key entry + model listing + a status readout.

**The conversion is done.** `custom`, `openrouter`, `kilocode` (including the
balance readout, which moved out of `lib/model_probe.cpp` and the TUI into the
plugin) and `anthropic` are plugins; `capability_overrides()`,
`ProviderCapabilities` and the static preset repository are deleted, and
`flavor` is a field of the provider definition, round-tripped through
`~/.config/amber/providers/*.conf`. The core registers no provider at all,
with no plugins loaded, `/provider list` is empty, and `lib/dialect.cpp`
registers only the transport's own `openai` protocol, so a disabled provider
plugin takes its protocol out of the table with it. The file layer stays: a
user's own `~/.config/amber/providers/<name>.conf` is data about a provider,
independent of the preset that names it.

**The wallet is framework-owned on purpose.** A provider's balance could be
done per plugin (kilocode did exactly that: its own poll loop, atomic cache and
status segment). It is not, because the part that varies is a single HTTP call
and everything else, when to refresh, how to cache, how to render, how to drop
under pressure, should be identical for every provider. A plugin supplies the
fetch; a provider without one simply reports `-`.

Two providers declare wallets today, and they show why the fetch is the right
seam: kilocode reads an account balance (`api.kilo.ai/api/profile/balance`,
authenticated with the gateway key), while OpenRouter reports a *per-key* spend
cap (`GET /v1/key` → `limit_remaining`, visible to an ordinary inference key).
A third kind of provider, one with no account API to ask, which is every
OpenAI-compatible endpoint including the user's own, declares nothing and
renders `-` rather than a fabricated number. Account-wide OpenRouter credits
need a management key, so they are deliberately out of scope; when that need
arrives it becomes the first consumer of host services (§8), not a wallet
special case. The readout refreshes when a
turn ends (a balance moves because we spent something), on a provider switch,
and once at startup, with a floor so a fast tool loop cannot hammer the
endpoint; the fetch runs off the UI thread. `/get provider wallet` reports the
state and value, using the same source as the bar.

**Flavor resolution is state-dependent, and the fallback must not lie.** A
flavor that no dialect implements (a typo in a provider file) falls back to
`openai`, that behaviour is correct and stays. A flavor that *a disabled plugin*
implements must **fail loudly**, naming the plugin and the fix
("`gemini` is provided by plugin `gemini`, which is disabled,
`/set plugin gemini on`"). Otherwise a user file (`~/.config/amber/providers/*.conf`,
which holds the key and survives the plugin being switched off) keeps looking
configured while silently speaking the wrong protocol. Unknown ⇒ fallback;
known-but-disabled ⇒ error.

**Fixed as a consequence:** `HttpModelCatalog` currently builds a bare config
and probes with the default dialect (`lib/providers_catalog_http.cpp:14-24`), so
`/provider test` and model listing are wrong for any non-OpenAI provider
(`https://api.anthropic.com/models` with a Bearer header). The catalog becomes
provider-driven: resolve the provider's dialect, use its `models_url` and
`auth_headers`, parse with `parse_model_list_response`. An empty `models_url`
means "no listing", reported as such, never as a false negative.

---

### 8. Host services (UI), not implemented

> **Status: not available.** This section is the agreed contract, not the
> shipped API. `PluginServices` exposes no `ui()`, and `HostServices` today
> carries only jobs, todos, sub-agents and the cancel token. Tracked as PF-3.3;
> do not build against it.

Plugins need the user: an API key, a choice, a confirmation, a notice. They get
**host-mediated verbs**, never widgets:

```cpp
class HostServices {
public:
    virtual std::string ask_text(const AskSpec&) = 0;     // blocking from the plugin's thread
    virtual std::string ask_secret(const AskSpec&) = 0;   // masked
    virtual int         choose(const ChooseSpec&) = 0;    // list selection, -1 on cancel
    virtual bool        confirm(const ConfirmSpec&) = 0;
    virtual void        notify(Level, const std::string&) = 0;
    virtual void        post_to_ui(std::function<void()>) = 0;  // mailbox, drained by the host tick
};
```

- **TUI implementation** reuses the existing machinery: widgets
  (`form_edit`, `menu_select`, `confirm_panel`, `info_dialog`) run on the UI
  thread while the requesting thread blocks on a future, exactly how the
  API-key prompt works today (`event_router.cpp:135-150` → `tui_input.cpp:1981`),
  including the modal-deferral queues.
- **CLI implementation** prompts on the TTY and denies non-interactively, matching
  the bash tool's approval contract.
- `post_to_ui` is the sanctioned cross-thread update path; the host drains it on
  its existing tick (the TUI's 50 ms loop, `widgets.h:72`).

**Provider key entry** is the first consumer: the auth flow may
call `ask_secret`, and 401/403 repair migrates from the hardcoded
`AgentHooks::on_api_key` path onto the same service. Plugins never learn what a
dialog is.

---

### 9. Composition root, lifecycle, state

**`PluginRuntime`** is the single composition root used by both hosts:

```
PluginRuntime
 ├─ ToolRegistry          (existing)
 ├─ ProviderService       (existing; presets merged from plugin contributions)
 ├─ PluginRegistry        (existing lifecycle; fed by the runtime)
 ├─ EventBus              (existing primitive + typed Events layer)
 ├─ extension registries  (prompts, status, panels, wallets, allowances)
 ├─ HostServices          (jobs, todos, sub-agents, cancel token)
 └─ PluginLedger
```

`src/main.cpp` (CLI) and `tui/tui_main.cpp` both construct a `PluginRuntime`,
call `add_bundled()`, `attach_host_services()`, `attach_config()` and `start()`,
so both hosts expose the same plugin surface. The earlier asymmetry where the
CLI had no plugin system is gone.

**Lifecycle:** `Discovered → Registered → (enable) Active → (disable)
Deactivated → Shutdown`, unchanged from the existing state machine
(`plugin_registry.cpp`). What changes is that activation installs capabilities
into the registries and records them in the ledger, and deactivation unwinds.

**State layout** (forward-compatible with a future external tier):

```
~/.config/amber/plugins/<id>/plugin.conf     # enabled=1, plugin settings (key=value)
$(datadir)/amber/plugins/<id>/               # bundled plugin assets (installed, read-only)
```

Bundled core plugins are compiled in today (no dlopen, no process boundary);
their *state* still lives in the per-plugin directory so that opening the
framework later does not change the layout users see. `$(datadir)` is
`<prefix>/share/amber` (`Makefile.in:413`), already the install target for
`prompts/` and `completions.json`, and already scanned by the external plugin
discovery roots (`lib/plugin.cpp:163-168`).

**Versioning:** not in this tier. A compiled-in plugin cannot be a different
version from the harness, the compiler enforces it, so no runtime version gate
exists until PF-6 admits code that was built elsewhere (D10).

---

### 10. Non-goals and deferred work

| Deferred | Why now |
|---|---|
| External (process) tier, PF-6 | The in-process port must be proven first (Gemini, PF-2) before its wire form is knowable; designing an IPC protocol for an unproven interface is speculative. Shaped for in the meantime: declarative capabilities, per-plugin state dir, no harness-internal dependencies. |
| Log sinks | Cut from PF-1: no phase named a consumer, and a registry that nothing drives is the failure mode this design exists to avoid. Reopens when a component needs structured log fan-out (the conversation log stays authoritative). |
| `kPluginApiVersion` gate | **Deferred to PF-6.** For compiled-in plugins the compiler is the version check; a runtime constant compared at registration does nothing. It earns its place only when code can arrive that was not built with the harness. |
| `dlopen` loadable libraries | **Rejected, not deferred** (D16): it buys "install without rebuilding" while costing a C++ ABI contract that is fragile across compilers and stdlib versions, and it keeps the shared crash domain. The process tier is strictly better for that use case. |
| Raw key interception | A plugin that can swallow arbitrary keys can make the UI unusable and is untestable. Panels receive keys when focused; commands are contributed declaratively. Revisit after the panel contract is proven. |
| Window geometry / z-order / arbitrary placement | The TUI has one full-screen layout with a single active window (`render_engine.cpp:25-26`, `window_manager.cpp:16-38`). Declared regions (segments, panels) cover the real requirements without a layout-engine rewrite. |
| Per-token stream events | Violates the performance invariants; the host UI already consumes tokens via `AgentHooks`. |
| Themes / arbitrary render hijack | `Theme` in the old draft painted inside ncurses internals. Deferred until the segment/panel contract is stable. |
| Hot reload, plugin dependencies | No consumer; adds lifecycle states with no benefit yet. |
| External plugins gaining non-tool capabilities | The external process protocol stays tools-only until the in-process framework is proven. |

---

### 11. Security

- **Core plugins are trusted code.** They run in-process with full harness
  access; enabling one is a build/ship decision, not a runtime one. This is why
  the framework ships compiled-in and why the docs must say so plainly.
- **External plugins are untrusted**: separate process, workspace-confined
  paths, 64 KiB output cap, tools only, `plugin_<id>_<name>` namespace. No
  change to that model.
- **Namespacing**: contributed commands live under the plugin's declared root;
  tools contributed by plugins keep an owner tag so the console and the ledger
  can attribute them. Nothing a plugin contributes is anonymous.
- **Capability audit**: the console lists every contribution with its owner, so
  "what did this plugin add to my harness" is answerable without reading code.

---

### 12. Threading contract

| Surface | Thread | Rule |
|---|---|---|
| Event handlers | the thread that published (agent owner thread, or UI thread for UI events) | Must not block. Long work goes to the plugin's own worker. |
| Status segment / panel render callables | UI thread only | Must be fast (they run inside frame composition) and **must be pure reads**: no mutation of plugin state, no I/O |
| `ask_*` / `choose` / `confirm` | called from the plugin's thread; the host shows the modal on the UI thread | Blocking on the caller side is expected and matches the existing approval/API-key pattern |
| `notify`, `post_to_ui` | any thread | Queued; drained by the host tick |
| Plugin-owned mutable state shared between event handlers and render | the plugin's responsibility | Sanctioned pattern: mutate from the event, then `post_to_ui` the UI-visible snapshot. Direct cross-thread reads are the plugin's bug, and the guide says so. |

The single-owner rule for `Context` is untouched: plugins never mutate the
context, and prompt blocks are rendered onto the prompt copy, not pushed onto
the sealed stack.

---

### 13. Testing strategy

1. **Registry unit tests**: install, order, introspection, removal for every
   extension point (hermetic, no host).
2. **Ledger tests**: a plugin contributing one of everything, disabled, leaves
   the registries byte-identical to their pre-activation state. This is the test
   that makes enable/disable real.
3. **Typed event tests**: subscribe/publish per payload type; the unsubscribed
   fast path returns without invoking anything; existing `event_bus_test.cpp`
   semantics (LIFO interceptors, re-entrancy) stay green.
4. **Prompt block tests**: ordering, determinism across turns, KV-prefix
   stability (same inputs → same prefix).
5. **Host surface tests**: segment rendering and panel key handling against a
   fake host; the TUI-specific parts in `tests/tui_tests.cpp`.
6. **Provider capability tests**: Gemini dialect body/parse/stream against
   fixtures (mirroring `tests/dialect_anthropic_test.cpp`), plus the
   provider-driven catalog behaviour for a provider without a listing endpoint.
7. **Performance guard**: a test asserting `publish()` with no subscribers is a
   no-op, and that a full turn with the framework active publishes nothing per
   token.
8. **End-to-end**: a harness test that boots `Runtime` with bundled plugins,
   drives a fake-LLM turn, and asserts the contributions are live and then gone
   after disable.

Every phase gate: `make clean && make && make test && make lint && make analyze
&& make check`, zero new clang-tidy/cppcheck findings, g++ and clang++.

---

### Invariants

These hold always. A test that cannot express one of them is a design smell.

1. **Install returns a handle.** Every contribution is removable through the
   handle the registry returned; there is no write-only registration.
2. **No orphan contributions.** After `disable(id)` returns, nothing the plugin
   contributed remains reachable, registries, subscriptions, command leaves,
   provider rows, segments, panels.
3. **Registration never happens through events.** Events notify; registries
   install.
4. **No `void*` in the plugin-facing API.** Events and capabilities are typed.
5. **Unsubscribed publish is free.** No handlers invoked, no allocation, no
   lock contention on the hot path.
6. **Nothing publishes per token.** Streaming stays with the host UI.
7. **Plugins never mutate `Context`.** Prompt contributions render onto the
   prompt copy; the sealed stack is untouched (see the context ownership spec).
8. **A disabled plugin costs nothing.** No subscriptions, no registry rows, no
   constructed dialect, no polling, the only residue is its `plugin.conf`.
9. **Plugin state has one source of truth.** Enable/disable and settings live in
   `~/.config/amber/plugins/<id>/plugin.conf`; commands read and write that, never
   a shadow copy.
10. **Every plugin surface is reachable from the command tree**: `/get plugin`
    and `/set plugin` are the control surface. No hidden state.
11. **Plugins depend only on the public plugin API.** No `tui/` includes, no
   harness-internal types, so moving the tier boundary later costs nothing.
12. **Flavor resolution is state-aware and race-free.** Unknown flavors fall
   back to `openai`; a flavor provided by a *disabled* plugin fails loudly,
   naming the plugin and the command that fixes it. Registry mutation (plugin
   enable/disable) never races a lookup: work already in flight completes with
   the objects it holds, and the next resolution sees the new state.

---

### Scenarios

#### [PLG-11] Plugin registry is a get/set surface

- **Given**: bundled plugins registered, some disabled in `plugin.conf`
- **Input**: `/get plugin list`; `/set plugin off <id>`; restart; `/set plugin on <id>`
- **Expected**: the list shows every plugin with tier, state and contributions;
  the toggle persists and survives restart; `/get plugin info <id>` reports the
  api version, state source and manifest; a disabled plugin's contributions are
  absent; the drawers of `/get plugin` and `/set plugin` show commands, never
  the plugin ids.
- **Regression guard**: `plugin_getset_*` tests.

#### [PLG-12] Provider list reflects plugin state live

- **Given**: a provider contributed by an enabled plugin
- **Input**: `/get provider list`, then `/set plugin off <id>`, then `/get provider list` again
- **Expected**: the provider appears, then is gone, with no restart, and with
  the completion drawer agreeing with the list. Re-enabling restores it.
- **Regression guard**: `provider_feed_follows_plugin_state`.

#### [PLG-13] A provider file survives its plugin

- **Given**: a provider whose `flavor` is supplied by a plugin, with a user
  provider file (`~/.config/amber/providers/<name>.conf`) holding its key
- **Input**: `/set plugin off <id>`, then a turn against that provider
- **Expected**: the turn fails loudly, naming the plugin and the command that
  re-enables it, **never** a silent fallback to the OpenAI dialect. Re-enabling
  restores normal operation.
- **Regression guard**: `disabled_flavor_fails_loudly`, `flavor_fallback_is_unknown_only`.

#### [PLG-01] Activation installs, deactivation removes

- **Given**: a runtime with an empty harness
- **Input**: enable a plugin contributing a tool, a prompt block, a status
  segment, and an event subscription; then disable it
- **Expected**: every contribution is observable while active and absent after
  disable; the registries are in their pre-activation state; no callback fires
  afterwards.
- **Regression guard**: `plugin_ledger_*` tests.

#### [PLG-02] Plugin command leaves execute, not applicable

- **Status**: not applicable. The command-contribution capability was removed
  (no producer, no consumer); `CapabilityKind` has no `Command`. This scenario
  is retained as a record and reopens only if a command capability returns.
- **Historical expectation**: a plugin contributing a command subtree would have
  its handler run when the deepest leaf is dispatched from the drawer.

#### [PLG-03] Typed events carry payloads

- **Given**: a subscriber for `ToolRequested`
- **Input**: an agent turn that requests a tool
- **Expected**: exactly one event per requested call, before the approval gate,
  with the live args; no event for hidden confirmation exchanges.
- **Regression guard**: `event_tool_requested_*` tests.

#### [PLG-04] Unsubscribed publish is free

- **Given**: no subscribers
- **Input**: publish every event type in a loop
- **Expected**: no handler invocation, no allocation, no observable cost,
  asserted by a no-op test and by the absence of per-token publishing.
- **Regression guard**: `publish_without_subscribers_is_noop`.

#### [PLG-05] Prompt blocks are ordered and stable

- **Given**: core blocks plus a plugin block
- **Input**: two consecutive turns
- **Expected**: identical block order and identical rendered prefix when inputs
  are unchanged; plugin blocks land at their declared priority.
- **Regression guard**: `prompt_blocks_*` tests.

#### [PLG-06] Status and panels come from the registry

- **Given**: a registered segment and panel
- **Input**: compose a frame
- **Expected**: the segment appears in priority order and drops in the declared
  overflow order; the panel renders and receives keys when focused.
- **Regression guard**: `status_registry_*`, `panel_registry_*` tests.

#### [PLG-07] Plugin-mediated user input

- **Given**: a plugin calling `ask_secret` from a worker thread
- **Input**: the host raises the modal; the user submits
- **Expected**: the value returns to the caller; an API-key request arriving
  during an open modal is queued, not nested; the flow works in CLI and TUI.
- **Regression guard**: `host_services_*` tests.

#### [PLG-08] Provider plugin is complete without core edits

- **Given**: a new provider plugin (Gemini)
- **Input**: enable it, select the provider, list models, send a turn
- **Expected**: endpoints, auth, body, streaming, usage, and model listing are
  correct; `/provider test` succeeds; **no file outside the plugin's own sources
  and the registry row is modified**.
- **Regression guard**: `tests/dialect_gemini_test.cpp`, provider plugin tests.

#### [PLG-09] Provider without a listing endpoint is honest

- **Given**: a provider whose dialect declares no model listing
- **Input**: `/provider test`, model feed refresh
- **Expected**: reported as "no model listing", not an empty failure and not a
  false negative.
- **Regression guard**: `catalog_without_listing_*` test.

#### [PLG-10] Disable is a first-class state

- **Given**: a plugin disabled in `plugin.conf`
- **Input**: start the harness
- **Expected**: the plugin is not registered as active; its contributions are
  absent; the console shows it as disabled; re-enabling restores them without
  restart (where the contribution allows it).
- **Regression guard**: `plugin_state_persistence_*` tests.

---

### Cross-references

- **Depends on**: `llm-client/dialect.md` (provider wire seam),
  `agent-loop/core-loop.md` (turn boundaries), `context/context-ownership-and-parallel-compression.md`
  (single-owner rule, prompt-copy augmentation), `tui/layout-engine.md`
  (regions), `config/file-config.md` (state layout conventions).
- **Depended on by**: `plugins/tools-domain.md` (splitting the tool set into
  plugins, per-tool meta and prompts, the next domain to move),
  `plugins/developer-guide.md` (author-facing contract), `plugins/README.md`
  (external tier, unchanged), `llm-client/model-probe.md`
  (provider-driven catalog).
- **Tracker**: `docs/plugin-framework-tracker.md` (phases PF-1..PF-5, decision
  log, deferred register).
- **Test coverage**: registry and ledger tests (new), `tests/plugin_core_test.cpp`,
  `tests/plugin_framework_test.cpp`, `tests/plugin_runtime_test.cpp`,
  `tests/event_bus_test.cpp`, `tests/agent_events_test.cpp`,
  `tests/metrics_plugin_test.cpp`, `tests/dialect_*_test.cpp`,
  `tests/tui_tests.cpp`.

### Revision history

| Date | Reason |
|------|--------|
| 2026-09-09 | Initial framework draft (aspirational: `void*` capabilities, untyped bus, capability routing, theme/UI hijack, Phase 1 only) |
| 2026-09-10 | Rebuilt as the agreed architecture: typed capabilities + ledger, typed events with real fire sites, three-mechanism model, declared UI regions, host services, composition root, provider capability as first consumer, performance invariants, deferred register |
| 2026-09-12 | Aligned with the shipped API: commands and settings removed, host services marked not implemented, `PluginRuntime` named, external tier renamed, "v2" dropped in favour of amber's own versioning |
