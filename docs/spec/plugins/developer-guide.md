# Amber Plugin Developer Guide

How to extend amber. Two plugin tiers exist, and they are different products
with different trade-offs, pick deliberately:

| | **Core plugin** | **External plugin** |
|---|---|---|
| Ships today | ✅ Yes | ✅ Yes |
| Language | C++17, compiled into amber | Any (executable) |
| Isolation | Same process; a crash takes amber down | Separate process; a crash cannot take amber down |
| Can contribute | Tools, providers, prompt blocks, status segments, panels, wallets, allowances, event subscriptions | Tools |
| Distribution | Compiled in, `plugins/<id>/` | User installs into `~/.config/amber/plugins/<id>/` |
| Spec | `plugins/plugin-framework.md` | `plugins/README.md` (wire protocol), this guide §6 |

There is no separate "plugin framework version". The framework ships as part of
amber and follows amber's own version (`version.txt`). The external tier has its
own wire `protocol_version` (currently `1`); that is a protocol number, not a
framework version.

**The framework is dogfooded.** Amber's own providers, its core tool set, the
status bar and the registry console are all built on the same capability API a
plugin uses. If you are here to add an LLM provider, read §5, that path is the
reason the framework exists.

---

## Quickstart: your first plugin

A complete, compiled, tested example lives in `examples/plugin_hello/`, its
test is `tests/example_plugin_test.cpp`, and it is built by `make test`. Copy it
and change the id.

1. **Create the directory** `plugins/<id>/` with `<id>_plugin.h` and
   `<id>_plugin.cpp`.
2. **Implement `IPlugin`** (§1).
3. **Declare a capability** in `capabilities()` (§2).
4. **Register it** in `make_bundled_plugins()` (`lib/plugins_bundled.cpp`) and
   add the object to `Makefile.in` (the compile rule is generic).
5. **Test it hermetically** (§8), no network, ever.
6. **Prove the seam held:** your diff must not touch `lib/http_transport.cpp`,
   the agent loop, or the TUI. If it does, the framework, not your plugin, is
   missing an extension point; say so in the PR instead of working around it.

### Which tier do I want?

```
Does it need to run arbitrary/other-language code, or must a crash be contained?
  └─ yes → External plugin (separate process, tools only)          §6
  └─ no
      ├─ Does it contribute anything other than a tool
      │  (provider, status segment, panel, prompt block, event hook)?
      │     └─ yes → Core plugin (C++17, compiled in)              §1
      └─ Is the endpoint OpenAI-wire compatible?
            └─ yes → you may not need code at all: a provider *file*
                      is enough (user-side config, no plugin)       §5
```

---

## Availability

This table is the single status surface for the framework. It reflects the code
on `main`; if it disagrees with the code, the code is right and the table is a
bug.

| Capability | Status | Notes |
|---|---|---|
| External tool plugin (subprocess, JSON-RPC) | ✅ Available | `plugins/README.md` |
| Tool contribution (incl. the core tool set) | ✅ Available | `ToolCapability` |
| Harness services in capability factories (`HostServices`) | ✅ Available | jobs, todos, sub-agents, cancel token |
| Typed event subscription | ✅ Available | `Events`, §3 |
| Prompt block contribution | ✅ Available | `PromptBlockCapability` |
| External plugins under the unified registry | ✅ Available | `ExternalPluginAdapter` |
| `/get plugin`, `/set plugin <id> on\|off` | ✅ Available | single write path |
| Provider contribution (dialect + presets) | ✅ Available | `ProviderCapability` |
| Status segment contribution | ✅ Available | `StatusSegmentCapability` |
| Panel contribution + registry console | ✅ Available | `PanelCapability` |
| Wallet readout | ✅ Available | `WalletCapability` |
| Command contribution (slash namespace) | ✅ Available | `CommandCapability` |
| Host services to the user (`ask_secret`, `choose`, `confirm`, `notify`) | ✅ Available | `PluginServices::ui` |
| Per-plugin settings | – | Not available, use `~/.config/amber/plugins/<id>/plugin.conf` |
| Log sinks | – | Deferred (no consumer) |
| `PluginLoaded` / `PluginUnloaded` events | – | Declared in `events.h`, never published |
| Theme, key interception, window geometry, hot reload | – | Deferred register (tracker) |

---

## 1. Core plugin anatomy

A core plugin is a C++ class implementing `IPlugin` plus the capabilities it
declares:

```cpp
#include "agent/plugin_core.h"

class HelloPlugin : public agent::IPlugin {
public:
    std::string id() const override { return "hello"; }        // slug, unique
    std::string version() const override { return "1.0.0"; }   // semver
    std::string name() const override { return "Hello"; }      // display

    // Optional metadata, shown in the registry list (/get plugin list, Alt+0).
    // The description answers "which one do I want", keep it to one line.
    // The category is the group it appears under; the vocabulary in
    // agent::plugin_category is the common set, and your own value works too.
    std::string description() const override {
        return "Greets people by name.";
    }
    std::string category() const override {
        return agent::plugin_category::kTools;
    }

    bool initialize(const agent::PluginContext& ctx) override {
        ctx_ = &ctx;          // store; do not do work here that needs installing
        return true;          // false => plugin marked Failed, nothing installed
    }

    void shutdown() override {
        // Release plugin-owned resources. Contributions are removed by the
        // runtime's ledger, do not unregister them yourself.
    }

    std::vector<std::unique_ptr<agent::Capability>> capabilities() override;
};
```

The runtime installs your capabilities, records each one in the ledger, and on
disable unwinds them in reverse order. **You declare; the runtime installs.** A
plugin that registers things outside this path is a bug, because it cannot be
disabled cleanly.

Bundled plugins live in `plugins/<id>/` and are registered in one place
(`make_bundled_plugins()` in `lib/plugins_bundled.cpp`), so the shipped set is
enumerable at a glance.

Neither `description()` nor `category()` is required, a plugin that declares
neither still works and appears under `other` with an explicit
`(no description)` marker, so an omission is visible rather than silent. There
is no runtime version check for core plugins: they are compiled into the same
binary, so the compiler is the version check.

`tick()` is called periodically on the UI thread for time-driven work (polling a
balance, refreshing a remote value). It exists so that *rendering* stays a pure
read. It must not block, schedule slow work on your own thread.

---

## 2. Capabilities

One capability = one contribution. `kind()` tells the runtime which registry
installs it; `install()` returns a handle the ledger keeps.

```cpp
class Capability {
public:
    virtual ~Capability() = default;
    virtual std::string name() const = 0;       // unique within the plugin
    virtual CapabilityKind kind() const = 0;
    virtual InstallResult install(PluginServices&) = 0;
};
```

`CapabilityKind` has eight values: `Tool`, `PromptBlock`, `StatusSegment`,
`Panel`, `Provider`, `Wallet`, `Allowance`, `Command`. Each has a concrete base
class in `include/agent/extensions.h`.

`InstallResult` distinguishes a **decline** from a **failure**:
`ok == false && declined == true` means "nothing to install" and the plugin
stays active; `ok == false && !declined` means "broken". A factory returning an
empty list declines.

### Tool

A tool is contributed with `ToolCapability`. Give it a factory when the tool
needs harness services at construction, the core tool set does, because bash
binds to the job service and todowrite to the todo store.

```cpp
std::vector<std::unique_ptr<agent::Capability>> GreetPlugin::capabilities() {
    std::vector<std::unique_ptr<agent::Capability>> caps;
    caps.push_back(std::make_unique<agent::ToolCapability>(
        "greet", [](agent::PluginServices& svc) -> std::vector<std::unique_ptr<agent::Tool>> {
            std::vector<std::unique_ptr<agent::Tool>> tools;
            tools.push_back(std::make_unique<GreetToolImpl>());
            return tools;
        }));
    return caps;
}
```

A factory returning an empty list *declines*: the tool is simply absent and the
plugin stays active. That is the shape a tool gated on configuration uses (the
core tool set ships `todowrite` and `task` that way). Returning several tools
makes them one contribution, the ledger records one entry, so disabling the
plugin takes them all back out together.

Rules: return errors as `ToolResult{false, "", error}`, never throw
(`AGENTS.md` error conventions). Tool ownership is recorded by the registry, so
unwinding is identity-scoped: a plugin can never remove another plugin's tool.

### Prompt block

```cpp
class ProjectFactsBlock : public agent::PromptBlockCapability {
public:
    std::string id() const override { return "project_facts"; }
    int priority() const override { return 400; }   // ascending; core uses 100/200/300

    std::string render(const agent::PromptSnapshot&) const override {
        return has_changed() ? "Project facts: …" : "";   // "" contributes nothing
    }
};
```

Blocks are appended as their own `system` message on the **prompt copy**: the
sealed `Context` is never mutated. **Determinism matters:** a block whose content
changes every turn invalidates the server's KV prefix from its position onward.
Keep volatile content at a high priority (near the tail), or cache until the
underlying fact changes.

### Status segment

```cpp
class BalanceSegment : public agent::StatusSegmentCapability {
public:
    std::string id() const override { return "kilocode_balance"; }
    int priority() const override { return 500; }
    int drop_priority() const override { return 10; }  // higher drops last

    std::string text(const agent::StatusSnapshot&) const override {
        return balance_ < 0 ? "" : "$" + fmt(balance_);
    }
};
```

Render callables run **inside frame composition on the UI thread**: fast, pure
reads, no I/O, no locks, no mutation. Update the value from an event handler and
publish the UI-visible snapshot with `post_to_ui` if needed.

### Panel

A panel is text plus optional key handling. The host frames it, scrolls it and
offers cycling between panels (Tab); your `on_key` gets first refusal on every
key while your panel is focused, and returning true means "I handled it".

```cpp
agent::PanelSpec spec;
spec.id = "gemini_models";
spec.title = "Gemini models";
spec.lines = [this](int width) { return model_lines(width); };  // pure
spec.on_key = [](int key) { return key == 'r'; };               // optional
caps.push_back(std::make_unique<agent::PanelCapability>(std::move(spec)));
```

`lines(width)` is called on every repaint, so keep it pure and fast, no I/O,
no blocking. The registry console at Alt+0 is the worked example.

### Command

A plugin contributes a slash-command namespace: a root, its help/man, the child
nodes (completions.json shape), and one handler per executable leaf. The host
merges the subtree into the command tree and binds each leaf to its handler.

```cpp
agent::CommandSpec spec;
spec.root = "hello";
spec.help = "Say hello";
spec.man  = "Usage: /hello greet <name>";
spec.subtree = agent::json::parse(R"({
  "greet": {"help": "Greet someone by name"}
})");
spec.handlers["greet"] = [](const std::string& arg) {
    return "Hello, " + (arg.empty() ? std::string("world") : arg) + "!";
};
caps.push_back(std::make_unique<agent::CommandCapability>(std::move(spec)));
```

- The handler returns the text to display; the host prints it. That keeps the
  plugin UI-free, no `tui/` includes.
- `handlers` is keyed by the leaf's **dotted path** relative to the root
  (`"greet"`, `"config.set"`). A leaf with no handler stays inert: documented in
  the drawer, not executable.
- You never name an action string. The host derives it
  (`plugin.<id>.<path>`), so a plugin command cannot collide with a core one.
- The **TUI** owns the slash engine; the headless CLI has no command tree, so a
  command contribution is TUI-only today.
- Toggling the plugin rebuilds the tree immediately, its commands appear or
  disappear with no restart.

The bundled `plugins/hello/` is the worked example: `/hello greet amber`
returns `Hello, amber!`.

### Provider

```cpp
agent::ProviderCapability::Preset preset;
preset.name = "gemini";
preset.api_base = "https://generativelanguage.googleapis.com";
preset.default_model = "gemini-2.5-pro";
preset.requires_key = true;

// Own protocol: provide the dialect factory.
caps.push_back(std::make_unique<agent::ProviderCapability>(
    "gemini", [] { return make_gemini_dialect(); },
    std::vector<agent::ProviderCapability::Preset>{preset}));

// Shared protocol (an OpenAI-compatible gateway): pass NO factory, so
// disabling the plugin cannot take the shared protocol down with it.
caps.push_back(std::make_unique<agent::ProviderCapability>(
    "openai", std::function<std::unique_ptr<agent::Dialect>()>{},
    std::vector<agent::ProviderCapability::Preset>{preset}));
```

Everything wire-specific, endpoints, auth headers, body, buffered parse, stream
decoding, model listing, usage mapping, retry classification, overflow hints,
belongs in the **dialect** (`docs/spec/llm-client/dialect.md`), not in the
plugin's plumbing. A provider plugin must not open its own HTTP client: the
transport is shared.

**Reading the host's configuration:** keep the `PluginContext` you are given
and read `ctx.config` (a pointer) at the moment you need it, never cache the
`Config*` in a member. The host may attach the configuration it actually
mutates *after* your plugin was activated (the TUI takes its `Config` by
value), so a cached pointer keeps pointing at the runtime's startup copy and
your feature silently stops working. This cost us the kilocode balance readout
once; the regression test is
`runtime_plugin_sees_the_hosts_config_attached_after_start`.

### Wallet (a provider's account state)

If your provider knows anything about the account behind the active key, a
prepaid balance, a set of quota windows, or both, declare how to fetch it. That
is the whole contribution. Do **not** write a poll loop, a cache or a status
segment: the runtime owns all of that, so your readout behaves exactly like every
other provider's.

One capability answers one question, "what is left?", in whatever shape your
provider reports. A provider that knows a number:

```cpp
caps.push_back(std::make_unique<agent::WalletCapability>(
    [](const agent::Config& cfg) -> std::optional<agent::WalletSnapshot> {
        const std::string token = my_balance_token(cfg);
        if (token.empty()) return std::nullopt;      // nothing to fetch with
        const double amount = my_fetch(token);
        if (amount < 0) return std::nullopt;         // failed: claim nothing
        return agent::WalletSnapshot::of_balance(amount);
    }));
```

A provider that meters usage windows returns them instead:

```cpp
caps.push_back(std::make_unique<agent::WalletCapability>(
    [](const agent::Config& cfg) -> std::optional<agent::WalletSnapshot> {
        if (cfg.api_key.empty()) return std::nullopt;
        const auto body = agent::http_get_with_bearer(url, cfg.api_key);
        if (!body) return std::nullopt;
        return my_parse_usage(*body);  // WalletSnapshot or nullopt
    }));
```

`WalletSnapshot` carries:

- `plan`, the plan name ("Pro", "Go", "free").
- `windows`, a vector of `WalletWindow`, each with a `label` ("5h", "7d",
  "monthly"), `percent_used` (0–100, -1 = unknown), `remaining` and
  `entitlement` (count or credits, -1 = not applicable), and `resets_at` (ISO
  8601 or empty).
- `credits_balance`, optional prepaid balance.
- `unit` and `currency`, what the numbers mean ("credits", "USD", "CNY").

Every field is optional, and filling only some of them is normal. What the bar
shows follows one rule: **the balance when there is one, otherwise the window
closest to interrupting current work** (highest `percent_used`; ties broken by
shortest label, so 5h beats 7d beats monthly). `/get provider wallet` shows the
whole picture, plan, every window, the balance, and
`/set provider wallet on|off` is the global display switch.

Return `nullopt` on any failure (no key, endpoint down, parse error), the bar
shows `-`, never a fake zero. The fetch runs off the UI thread and is called with
the **live** config, at most once per turn boundary plus startup and provider
switches. Do not block indefinitely; it is a network call you own.

### Runtime state: on/off and settings

Core plugins are **on by default**. Users control them through the command
tree, not through a config file edit:

| Command | Effect |
|---|---|
| `/get plugin list` | Every plugin with tier, state, and what it contributes |
| `/get plugin <id>` | Detail: version, capabilities, state source |
| `/set plugin <id> off` \| `on` | Enable/disable; applies immediately and persists |
| `/set plugin <id> <key>=<value>` | Per-plugin settings |

State lives in `~/.config/amber/plugins/<id>/plugin.conf`. Your plugin does not
read or write that file, the runtime does, and a disabled plugin is never
initialized.

When a plugin contributes providers, toggling it re-publishes the provider feed:
its providers appear in `/get provider list` and the completion drawer when on,
and are gone when off, with no restart. If your contribution is expected to be
visible live, say so in your PR description and add the feed-refresh test.

---

## 3. Events

Events are **typed**. Subscribe through the `Events` facade over the event bus,
and keep the returned `Subscription`, it is RAII, so dropping it unsubscribes:

```cpp
#include "agent/events.h"

class TurnCounter : public agent::IPlugin {
public:
    bool initialize(const agent::PluginContext& ctx) override {
        sub_ = agent::Events(ctx.event_bus)
                   .subscribe<agent::TurnEndedEvent>(
                       [this](const agent::TurnEndedEvent&) { ++turns_; });
        return true;
    }
    void shutdown() override { sub_.release(); }   // or just let it drop

private:
    agent::Subscription sub_;
    int turns_ = 0;
};
```

Interceptors run before observers and may mutate a payload or cancel the event;
returning `false` cancels it:

```cpp
sub_ = agent::Events(ctx.event_bus).intercept<agent::ToolRequestedEvent>(
    [](agent::ToolRequestedEvent& e) { return e.name != "danger"; });
```

Catalogue. **Fire sites are in `lib/agent.cpp` (loop) and `lib/dispatch.cpp`
(tools)** unless noted:

| Event | Fires? | Notes |
|---|---|---|
| `TurnStartedEvent` | ✅ | payload carries the prompt (interceptable) |
| `TurnEndedEvent` | ✅ | `cancelled` flag |
| `MessageAddedEvent` | ✅ | every `Context::push` in loop and dispatch |
| `ToolRequestedEvent` | ✅ | before the approval gate; interceptable (args, cancel) |
| `ToolCompletedEvent` | ✅ | after execution; carries the result and duration |
| `LlmResponseEvent` | ✅ | status + token counts |
| `CompressionEvent` | ✅ | compression triggered |
| `CompressionCompletedEvent` | ✅ | compression finished |
| `ErrorRaisedEvent` | ✅ | observation only; classification stays in the dialect |
| `PluginLoadedEvent` | ❌ | declared, never published |
| `PluginUnloadedEvent` | ❌ | declared, never published |

What you can rely on:

- Payloads are **typed**: no `void*` casts.
- Handlers run on the **thread that published** (the agent's owner thread for
  turn/tool/LLM events). Do not block. Long work goes to your own worker.
- `ToolRequested` fires **before** the approval gate: it means "the model asked",
  not "this will run".
- Hidden confirmation exchanges are never published.
- **There is no per-token event.** Streaming tokens remain the host UI's channel;
  a bus that fires per token would be a performance regression by design.
- Unsubscribed `publish()` is a single atomic load, cheap by construction.

---

## 4. Host services (talking to the user)

The agreed contract is a `PluginServices`-hosted UI service:

```cpp
// In a capability factory (or anywhere you hold a PluginServices&):
std::string key = services.ui->ask_secret({"Gemini API key", "Paste the key", ""});
int choice = services.ui->choose({"Pick a model", model_ids, 0});
bool ok = services.ui->confirm({"Overwrite the config?", "This cannot be undone"});
services.ui->notify(agent::UiLevel::Info, "Balance refreshed");
services.ui->post_to_ui([this] { snapshot_ = build_snapshot(); });
```

- Calls are blocking on *your* thread; the host shows the UI on its own thread
  and returns the answer. In a non-interactive CLI run they fail closed, the
  same way the bash tool's approval does.
- Use `post_to_ui` for anything the render callables will read: that is the
  sanctioned cross-thread update path.
- Never assume a terminal. Your plugin must work in the headless CLI.

---

## 5. Adding a provider (the flagship path)

**Every provider amber ships is a plugin**: there is no core provider list to
add to. `plugins/` holds the nine shipped providers, each small enough to read
in one sitting and each a template for the next:

| Plugin | What it shows |
|---|---|
| `plugins/custom/` | Presets only, no endpoint: the user's own endpoint, configured by file |
| `plugins/openrouter/` | Minimal vendor on a shared protocol (presets only, no dialect) + a per-key wallet |
| `plugins/kilocode/` | Shared protocol + a provider-specific feature (a wallet: one fetch, no poll loop) |
| `plugins/opencode_go/` | Shared protocol + an allowance (subscription usage windows) |
| `plugins/opencode_zen/` | Shared protocol, presets only (compatible models) |
| `plugins/commandcode/` | Shared protocol + an allowance (5h/weekly/monthly windows) |
| `plugins/deepseek/` | Shared protocol + a wallet (prepaid balance) |
| `plugins/anthropic/` | A vendor protocol the plugin itself provides |
| `plugins/gemini/` | A vendor protocol with a different streaming model, usage shape and model listing |

The other bundled plugins are not providers. The tool set is one plugin per
tunable unit, so a user can switch off exactly the tool they want to experiment
with: `plugins/tool_search/`, `plugins/tool_read/`, `plugins/tool_write/`,
`plugins/tool_bash/`, `plugins/tool_process/`, `plugins/tool_plan/` and
`plugins/tool_task/`. Alongside them: `plugins/metrics/` (an event-observing
plugin with no contributions) and `plugins/hello/` (the command example). That
is 18 bundled plugins in total.

Steps:

1. **Config-only first.** If the endpoint speaks the OpenAI wire protocol
   (`/chat/completions`, bearer auth, OpenAI SSE), a *user* needs no code at all:
   a provider file is enough. A plugin is for what amber should ship.
2. **New plugin directory** (`plugins/<id>/`, plus its object in `Makefile.in`;
   the compile rule is generic).
3. **Decide the dialect question:** does the endpoint speak the shared `openai`
   protocol (pass no dialect factory: presets only) or its own (write the
   dialect: `chat_url`, `models_url`, `auth_headers`, `build_chat_body`,
   `parse_completion`, `make_decoder`, `parse_models_response`, `parse_usage`,
   `is_retryable`, `context_overflow_hint`)?
4. **Declare the capability** and add it to `make_bundled_plugins()`.
5. **Test it hermetically**: body/parse/stream fixtures like
   `tests/dialect_gemini_test.cpp`, plus a registration test. No live network in
   `make test`.
6. **Prove the seam held:** your diff must not touch `lib/http_transport.cpp`,
   the agent loop, or the TUI. If it does, the framework, not your plugin, is
   missing an extension point; say so in the PR instead of working around it.

---

## 6. External plugins (available today)

External plugins are executables speaking newline-delimited JSON-RPC 2.0 over
stdio. Full protocol: **`plugins/README.md`**. Minimal shape:

```
~/.config/amber/plugins/hello/
├── manifest.json      # id, name, version, protocol_version, main, tools[], completion{}
└── hello-plugin       # executable, chmod +x
```

Methods: `initialize`, `tool.call`, `shutdown` (others return
`Method not found`). A tool call returns the standard envelope:

```json
{"id": 2, "result": {"ok": true, "output": "Hello, world!", "meta": {}}}
```

Manage with `/plugin list|status|enable|disable|install|uninstall`. Tools appear
to the agent as `plugin_<id>_<name>`; the agent prompt advertises them when the
plugin is enabled.

**Known limitation:** command subtrees contributed by external plugins render in
the completion drawer but cannot execute, no handler is bound for their
`plugin.*` actions. Handler binding is a **core plugin** extension point
(`CommandCapability`, `plugin-framework.md`); it does not reach the subprocess
tier, which contributes tools only. Treat external plugin commands as
documented-but-inert and expose behaviour through tools.

---

## 7. Rules the framework enforces

| Rule | Why |
|---|---|
| Declare capabilities; let the runtime install them | Only the ledger can guarantee clean disable |
| No screen access, no ncurses, no widget calls | Plugins must run headless; UI goes through host services |
| Render callables are pure and fast | They run inside frame composition |
| Never mutate `Context` | It is a sealed, hash-chained stack (`docs/spec/context/…`); prompt contributions go on the prompt copy |
| No per-token work | The streaming path is the hot path |
| No global key interception | Untestable and hostile to the user's UI |
| No blocking in event handlers | Events fire on the agent's owner thread |
| Errors are returned, not thrown | `ToolResult`/`InstallResult` conventions in `AGENTS.md` |

---

## 8. Testing your plugin

- **Unit**: capabilities against a fake `PluginServices`; assert what you
  contribute and that removal is total.
- **Hermetic integration**: register your plugin in a `Runtime`, drive a turn
  with the fake LLM client (`tests/fake_llm.h`), assert observable effects. No
  network, ever.
- **Dialect tests** (providers): pure body/parse/stream fixtures.
- **Regression**: run `make test`, your plugin's contributions must not leak
  into other tests (the ledger test exists to catch exactly this).

Every contribution lands with a failing test first (red), then the
implementation (green), then a PR that keeps
`make clean && make && make test && make lint && make analyze && make check`
green on g++ and clang++.

---

## 9. Review checklist for plugin PRs

- [ ] Plugin depends only on the public plugin API, no `tui/` includes, no
      reaching into `Tui` internals.
- [ ] Every contribution goes through a capability; nothing is registered in
      `initialize` outside the ledger.
- [ ] Render callables are pure; cross-thread state is synchronized by the host.
- [ ] No blocking, no I/O in event handlers.
- [ ] Tests: contribution, removal, and one hermetic end-to-end path.
- [ ] The availability table in this guide is updated in the same PR if a
      capability's status changed.
- [ ] Zero new clang-tidy/cppcheck findings; size limits respected (class ≤200
      lines, method ≤10 lines).

---

## 10. Where to look

| Question | Document |
|---|---|
| What is the framework, exactly? | `docs/spec/plugins/plugin-framework.md` |
| What is being built when? | `docs/plugin-framework-tracker.md` |
| Provider wire protocols | `docs/spec/llm-client/dialect.md` |
| External plugin protocol | `docs/spec/plugins/README.md` |
| Engineering standards, TDD workflow | `AGENTS.md` |
| Command-tree rules | `AGENTS.md` (command tree architecture) |
