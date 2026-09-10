# Amber Plugin Developer Guide

How to extend amber. Two plugin tiers exist, and they are different products
with different trade-offs — pick deliberately:

| | **External plugin** (v1) | **Core plugin** (v2) |
|---|---|---|
| Ships today | ✅ Yes | ⏳ In progress — see the availability table below |
| Language | Any (executable) | C++17, compiled into amber |
| Isolation | Separate process; a crash cannot take amber down | Same process; a crash takes amber down |
| Can contribute | Tools (+ inert command subtrees) | Tools, providers, commands, prompt blocks, status segments, panels, settings, log sinks, event hooks |
| Distribution | User installs into `~/.config/amber/plugins/<id>/` | Bundled with amber (compiled-in) |
| Spec | `plugins/README.md` (protocol), this guide §6 | `plugins/plugin-framework-v2.md` |

**The framework is being built to be dogfooded.** Providers, status readouts,
and the registry console are its first consumers. If you are here to add an LLM
provider, read §5 — that path is the reason the framework exists.

---

## Availability

The authority on status is `docs/plugin-framework-tracker.md`. This table mirrors
it; if they disagree, the tracker wins (and file a fix).

| Capability | Status | Phase |
|---|---|---|
| External tool plugin (subprocess, JSON-RPC) | ✅ Available | v1 |
| Tool contribution (core) | ⏳ | PF-1 |
| Command contribution, executable | ⏳ | PF-1 |
| Typed event subscription | ⏳ | PF-1 |
| Prompt block contribution | ⏳ | PF-1 |
| Settings contribution | ⏳ | PF-1 |
| v1 external plugins under the unified registry | ⏳ | PF-1 |
| `/get plugin`, `/set plugin on\|off` | ⏳ | PF-1 |
| Provider contribution | ⏳ | PF-2 |
| Status segment contribution | ⏳ | PF-3 |
| Panel contribution + registry console | ⏳ | PF-3 |
| Host services (`ask_secret`, `choose`, …) | ⏳ | PF-3 |
| Log sinks | – | Deferred (no consumer) |
| Theme, key interception, window geometry, hot reload | – | Deferred register |

Sections marked **⏳ target** describe the agreed contract. Do not build against
them until the tracker flips them to available — the spec is the design of
record, not a promise about `main`.

---

## 1. Core plugin anatomy

A core plugin is a C++ class implementing `IPlugin` plus the capabilities it
declares:

```cpp
#include "agent/plugin_v2.h"

class HelloPlugin : public agent::IPlugin {
public:
    std::string id() const override { return "hello"; }        // slug, unique
    std::string version() const override { return "1.0.0"; }   // semver
    std::string name() const override { return "Hello"; }      // display
    int api_version() const override { return agent::kPluginApiVersion; }

    bool initialize(const agent::PluginContext& ctx) override {
        ctx_ = &ctx;          // store; do not do work here that needs installing
        return true;          // false => plugin marked Failed, nothing installed
    }

    void shutdown() override {
        // Release plugin-owned resources. Contributions are removed by the
        // runtime's ledger — do not unregister them yourself.
    }

    std::vector<std::unique_ptr<agent::Capability>> capabilities() override;
};
```

The runtime installs your capabilities, records each one in the ledger, and on
disable unwinds them in reverse order. **You declare; the runtime installs.** A
plugin that registers things outside this path is a bug, because it cannot be
disabled cleanly.

Bundled plugins live in `plugins/<id>/` and are registered in one place
(`register_bundled_plugins`), so the shipped set is enumerable at a glance.

---

## 2. Capabilities ⏳ target

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

### Tool

```cpp
class GreetTool : public agent::Capability {
public:
    std::string name() const override { return "greet"; }
    agent::CapabilityKind kind() const override { return agent::CapabilityKind::Tool; }

    agent::InstallResult install(agent::PluginServices& svc) override {
        return svc.tools().add(std::unique_ptr<agent::Tool>(new GreetToolImpl));
    }
};
```

Rules: return errors as `ToolResult{false, "", error}` — never throw
(`AGENTS.md` error conventions). The tool name is namespaced so it cannot
collide with a core tool.

### Command

```cpp
agent::CommandSpec spec;
spec.root = "hello";                       // one namespace root, owned by you
spec.subtree = json::parse(R"({
  "greet": { "help": "Greet someone", "man": "Usage: /hello greet <name>" }
})");
spec.handlers["greet"] = [](const std::string& arg) { /* ... */ };
```

The runtime merges the subtree into the command tree and registers each leaf's
handler — leaf entries in the drawer always execute. The command surface stays
JSON-driven: never hardcode a path in a handler (`AGENTS.md`, command-tree
rules).

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

Blocks are appended as their own `system` message on the **prompt copy** — the
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

```cpp
class ConsolePanel : public agent::PanelCapability {
public:
    std::string id() const override { return "registry_console"; }
    std::string title() const override { return "Plugins"; }
    void render(agent::PanelCanvas& c, const agent::StatusSnapshot&) const override;
    bool handle_key(int key) override;   // true = consumed; called only when focused
};
```

The host owns placement, focus, and overflow — plugins never address the screen
directly. Keys arrive only while your panel is focused; the framework
deliberately does not allow global key interception (see the deferred register).

### Provider ⏳ target (PF-2)

```cpp
agent::ProviderSpec spec;
spec.flavor = "gemini";
spec.make_dialect = [] { return std::make_unique<GeminiDialect>(); };
spec.presets = {{"gemini", "https://generativelanguage.googleapis.com", "gemini-2.5-pro", true}};
spec.auth = agent::AuthSpec::api_key("GEMINI_API_KEY");   // may prompt via host services
```

Everything wire-specific — endpoints, auth headers, body, buffered parse, stream
decoding, model listing, usage mapping, retry classification, overflow hints —
belongs in the **dialect** (`docs/spec/llm-client/dialect.md`), not in the
plugin's plumbing. A provider plugin must not open its own HTTP client: the
transport is shared.

### Settings and log sinks

Settings contribute `/get`/`/set` entries with the same getter/setter contract
the core uses. Log sinks receive `(level, tag, message)`; they must be bounded,
non-blocking, and must not throw — the conversation log stays authoritative.

### Runtime state: on/off and settings

Bundled plugins are **on by default**. Users control them through the command
tree, not through a config file edit:

| Command | Effect |
|---|---|
| `/get plugin list` | Every plugin with tier, state, and what it contributes |
| `/get plugin <id>` | Detail: version, api version, capabilities, state source |
| `/set plugin <id> off` \| `on` | Enable/disable; applies immediately and persists |
| `/set plugin <id> <key>=<value>` | Per-plugin settings |

State lives in `~/.config/amber/plugins/<id>/plugin.conf` (the same directory a
user-installed external plugin already uses for its `manifest.json`). Your plugin
does not read or write that file — the runtime does, and a disabled plugin is
never initialized.

When a plugin contributes providers, toggling it re-publishes the provider feed:
its providers appear in `/get provider list` and the completion drawer when on,
and are gone when off, with no restart. If your contribution is expected to be
visible live, say so in your PR description and add the feed-refresh test.

---

## 3. Events ⏳ target

Subscribe through a `Hook` capability so the subscription is ledger-owned:

```cpp
class TurnCounter : public agent::HookCapability {
public:
    std::string name() const override { return "count_turns"; }
    void install(agent::PluginServices& svc) override {
        sub_ = svc.events().subscribe<agent::TurnEndedEvent>(
            [this](const agent::TurnEndedEvent&) { ++turns_; });
    }
};
```

Catalogue and fire sites: spec §6. What you can rely on:

- Payloads are **typed** — no `void*` casts.
- Handlers run on the **thread that published** (the agent's owner thread for
  turn/tool/LLM events). Do not block. Long work goes to your own worker.
- `ToolRequested` fires **before** the approval gate: it means "the model asked",
  not "this will run".
- Hidden confirmation exchanges are never published.
- **There is no per-token event.** Streaming tokens remain the host UI's channel;
  a bus that fires per token would be a performance regression by design.
- Error events are observation-only; classification (retryable? overflow?) is
  provider behaviour and lives in the dialect.

---

## 4. Host services (talking to the user) ⏳ target

```cpp
std::string key = ctx_->ui().ask_secret({"Gemini API key", "Paste the key"});
int choice = ctx_->ui().choose({"Pick a model", model_ids});
bool ok = ctx_->ui().confirm({"Overwrite the config?", "This cannot be undone"});
ctx_->ui().notify(agent::Level::Info, "Balance refreshed");
ctx_->ui().post_to_ui([this] { snapshot_ = build_snapshot(); });
```

- Calls are blocking on *your* thread; the host shows the UI on its own thread
  and returns the answer. In a non-interactive CLI run they fail closed, the
  same way the bash tool's approval does.
- Use `post_to_ui` for anything the render callables will read — that is the
  sanctioned cross-thread update path.
- Never assume a terminal. Your plugin must work in the headless CLI.

---

## 5. Adding a provider (the flagship path) ⏳ target

1. **Config-only first.** If the endpoint speaks the OpenAI wire protocol
   (`/chat/completions`, bearer auth, OpenAI SSE), it is **already supported** —
   add a provider definition, no code at all.
2. **A new wire protocol** is a plugin: write the dialect
   (`chat_url`, `models_url`, `auth_headers`, `build_chat_body`,
   `parse_completion`, `make_decoder`, `parse_models_response`, `parse_usage`,
   `is_retryable`, `context_overflow_hint`), then a `ProviderSpec` that registers
   it with presets and auth.
3. **Test it hermetically** — body/parse/stream fixtures like
   `tests/dialect_anthropic_test.cpp`. No live network in `make test`.
4. **Prove the seam held:** your diff must not touch `lib/http_transport.cpp`,
   the agent loop, or the TUI. If it does, the framework — not your plugin — is
   missing an extension point; say so in the PR instead of working around it.

The Gemini plugin (PF-2) is the worked reference implementation.

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

**Known limitation:** command subtrees contributed by external plugins currently
render in the completion drawer but cannot execute — no handler is registered for
`plugin.*` actions. Until PF-1 lands command contribution, treat external
plugin commands as documented-but-inert and rely on tools.

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
- **Regression**: run `make test` — your plugin's contributions must not leak
  into other tests (the ledger test exists to catch exactly this).

Every contribution lands with a failing test first (red), then the
implementation (green), then a PR that keeps
`make clean && make && make test && make lint && make analyze && make check`
green on g++ and clang++.

---

## 9. Review checklist for plugin PRs

- [ ] Plugin depends only on the public plugin API — no `tui/` includes, no
      reaching into `Tui` internals.
- [ ] Every contribution goes through a capability; nothing is registered in
      `initialize` outside the ledger.
- [ ] Render callables are pure; cross-thread state uses `post_to_ui`.
- [ ] No blocking, no I/O in event handlers.
- [ ] Tests: contribution, removal, and one hermetic end-to-end path.
- [ ] The availability table in the tracker is updated in the same PR.
- [ ] Zero new clang-tidy/cppcheck findings; size limits respected (class ≤200
      lines, method ≤10 lines).

---

## 10. Where to look

| Question | Document |
|---|---|
| What is the framework, exactly? | `docs/spec/plugins/plugin-framework-v2.md` |
| What is being built when? | `docs/plugin-framework-tracker.md` |
| Provider wire protocols | `docs/spec/llm-client/dialect.md` |
| External plugin protocol (v1) | `docs/spec/plugins/README.md` |
| Engineering standards, TDD workflow | `AGENTS.md` |
| Command-tree rules | `AGENTS.md` (command tree architecture) |
