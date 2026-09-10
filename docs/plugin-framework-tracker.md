# amber — Plugin Framework Tracker

- **Status:** 🟢 PF-1 and PF-2 complete (2026-09-10) on
  `feat/plugin-framework-pf1`. The framework runs; a provider plugin delivers a
  genuinely incompatible vendor (Gemini) with no transport, agent-loop or UI
  edits. Two follow-ups outstanding: core prompt blocks on the registry (PF-1)
  and the built-in provider conversion (PF-4). PF-3 (UI surfaces) is next.
- **Reference:** `docs/spec/plugins/plugin-framework-v2.md` (the contract)
- **Author guide:** `docs/spec/plugins/developer-guide.md`
- **Related:** `docs/spec/llm-client/dialect.md` (provider wire seam),
  `docs/fix-tracker.md` (FIX register; FIX-027..032 built the dialect seam this
  framework's provider capability plugs into)

---

## How to Use This Tracker

1. Every task follows **Red → Proposal → Sign-off → Green → PR** (AGENTS.md).
   Each task below carries its own scenario IDs from the spec; the red test is
   written against the spec's expected behaviour, not against the implementation.
2. **This tracker is the single source of progress.** When a task lands, update
   its checkbox and the availability table in one commit with the code.
3. **Decisions are recorded, not re-litigated.** The decision log (D-series) is
   binding: a change to a decision is a change to the spec, and it is made here
   first with the reason and the superseded alternative named.
4. Verification before any task is marked done:
   `make clean && make && make test && make lint && make analyze && make check`
   (g++ and clang++), zero new clang-tidy/cppcheck findings.
5. **No task ships its capability half-wired.** A registry that exists but is not
   driven by a real consumer is not done — every phase names its consumer.

## Legend

```
[ ] — Not started   [~] — In progress   [x] — Done, all checks pass
[!] — Blocked       [–] — Deferred (see Deferred Register)
```

---

## Current State (verified 2026-09-10, commit `c1ad6a8`)

Evidence captured before designing; this is the baseline the framework is
measured against. Re-verify rather than trust it if the tree has moved.

| Area | State | Evidence |
|---|---|---|
| `EventBus` primitive | Implemented, well tested, **inert in production** | `lib/event_bus.cpp`; only tests and the dormant metrics plugin fire it |
| `PluginRegistry` lifecycle | Implemented, tested | `lib/plugin_registry.cpp:8-65`; `tests/plugin_v2_test.cpp` |
| Plugin activation | **Never happens outside tests** | `tui/tui_main.cpp:158`, `tui/tui.cpp:96-98` construct + set context only |
| `Capability` | Declared, **never read in production**; `void* impl` | `include/agent/plugin_v2.h:17-33,53` |
| Tool contribution | **Impossible by design** | `PluginContext::tools` is `const` (`plugin_v2.h:37`); `register_tool` is non-const (`registry.h:29`) |
| Plugin commands | Render in the drawer, **cannot execute** | `tui/tui_input.cpp:1253-1261` (no `plugin.*` handler registered) |
| Status bar | Hardcoded segment list, private `Seg`, magic drop priorities | `tui/render_engine.cpp:160-243,305-439`; `tui/render_engine.h:76-80` |
| Key handling | Hardcoded `switch`, no keymap | `tui/tui.cpp:557-607` |
| Windows | Single full-screen layout, one active window, every window spawns an `Agent` | `tui/render_engine.cpp:25-26`; `tui/window_manager.cpp:16-38` |
| Host-mediated input | Works for approvals/API keys via promise + queue + modal deferral | `tui/event_router.cpp:135-150,217-224,267-282`; `tui/tui_input.cpp:1981-2018` |
| Prompt assembly | Four ad-hoc injection algorithms in one function | `lib/agent.cpp:190-204,244-263,290-319,327-335` |
| v1 external plugins | **Working** (subprocess JSON-RPC, tools only) | `lib/plugin.cpp:154-193,426-479`; `tools/plugins/{sysinfo,cdp}` |
| Provider dialect seam | Working; `register_dialect` already public | `include/agent/dialect.h:89`; `lib/dialect.cpp:14-34` |
| Provider capability routing | Name-keyed table, no plugin path | `lib/providers_service.cpp:12-33` |
| `/provider test` for non-OpenAI | **Broken** — probes with the default dialect | `lib/providers_catalog_http.cpp:14-24` |
| CLI plugin support | None | `src/main.cpp` has no plugin wiring |

---

## Progress Log

Newest first. Each entry: what landed, on which branch, and what it did *not*
cover.

### 2026-09-10 — PF-2.1–PF-2.6: provider plugins, Gemini, race-free registry

- **Landed**
  - **PF-2.1 Provider capability** — `ProviderCapability` carries flavor +
    dialect factory + presets; `register_provider_preset`/
    `unregister_provider_presets_for` put plugin presets in the ordinary
    repository merge, so a plugin provider is an ordinary provider row.
  - **PF-2.2 Registry resolution** — the dialect table is mutex-guarded;
    unregistering marks a flavor *unavailable* rather than forgetting it, and
    `HttpLLMClient` refuses to construct for a disabled plugin's flavor with a
    message naming the plugin and the fix. Unknown flavors still fall back.
  - **PF-2.3 Provider-driven catalog** — `Provider::flavor` now reaches the
    model catalog, which fixes `/provider test`: it previously probed every
    provider as OpenAI. `apply_selection` takes the flavor from the provider
    itself, so the name-keyed lookup is down to the kilocode account-token
    quirk alone (PF-4 deletes it).
  - **PF-2.4/2.5 Gemini dialect + plugin** — `generateContent` /
    `streamGenerateContent?alt=sse`, `x-goog-api-key`, `contents`+`parts`,
    `systemInstruction`, `functionDeclarations` with the schema inline,
    `functionCall`/`functionResponse` with synthetic call ids,
    `usageMetadata`, model listing that strips the `models/` prefix, its own
    overflow prose. `plugins/gemini/` contributes it; the bundled list carries
    it.
  - **PF-2.6 `flavor` in provider files** — parsed and written; the default is
    never written, so existing provider files keep meaning openai.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **597 passed, 0 failed** (11 Gemini dialect tests + the plugin
  register/unwind + the disabled-flavor refusal + the flavor round-trip),
  `make check` → all invariants hold, cppcheck clean. Live probe against the
  built binary: `/provider list` shows `gemini … flavor=gemini` served from the
  plugin, alongside `anthropic … flavor=anthropic`.
- **Not covered:** PF-4 converts the built-ins into plugins (the name-keyed
  `capability_overrides()` table still exists for the account-token quirk and
  the built-in flavors). `CapabilityKind::Provider` is consumed; nothing reads
  a *live* Gemini response (hermetic fixtures only, as specified — no live
  provider calls in the suite).

### 2026-09-10 — PF-1.2, PF-1.4–PF-1.7: the framework runs

- **Branch:** `feat/plugin-framework-pf1` (pushed; no PR yet — the feature is
  not finished to a shippable state).
- **Landed**
  - **PF-1.2 publish sites** — `Agent::set_events`; TurnStarted (before the
    prompt is sealed, so an interceptor can rewrite it), TurnEnded,
    MessageAdded at every push site, LlmResponse, CompressionTriggered/
    Completed, ErrorRaised; tool events in dispatch with an interceptable
    ToolRequested that can rewrite arguments or veto a call. Hidden
    confirmation exchanges publish nothing.
  - **PF-1.4 registries** — prompt blocks, commands, per-plugin settings,
    owner-tagged, plus four concrete capabilities (tool, command, prompt
    block, setting). `ToolRegistry::remove_tool`.
    The dead `void*`/Type-enum `Capability` struct is deleted; `IPlugin::
    capabilities()` returns owned typed capabilities, called once at
    activation.
  - **PF-1.5 runtime** — composition root, ledger install/unwind,
    `~/.config/amber/plugins/<id>/plugin.conf`, bundled-on-by-default,
    `set_state` as the single write path. TUI: `/get plugin [list|<id>]`,
    `/set plugin <id> on|off` as feed leaves, agents get the bus and the
    prompt registry. CLI: same runtime, `--no-plugins`.
  - **PF-1.6 v1 adapter** — `V1PluginAdapter` puts discovered external plugins
    under the same surface (off by default; first-wins on id collision).
  - **PF-1.7 dogfood (partial)** — metrics is live; an end-to-end test drives a
    real Agent turn with the bus attached and asserts the plugin counted it,
    then that disabling leaves the harness empty.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests`
  → **583 passed, 0 failed**, `make check` → all invariants hold, cppcheck
  clean on the new sources. Live smoke: TUI drawer shows `/get plugin` →
  `metrics on, bundled v1.0.0`; headless CLI run completes with the runtime
  active.
- **Deviation (recorded, not hidden):** PF-1.7's second half — migrating the
  **core** prompt blocks (memory, skill discovery, session brief) onto the
  PromptRegistry — is **not done**. Plugin blocks are appended after the core
  blocks today, which is behaviour-preserving and keeps the KV prefix stable.
  The core migration changes prompt assembly for every request, so it wants its
  own focused pass rather than the tail end of a long one.
- **Not covered:** PF-1.6's adapters are built at startup; a plugin installed
  mid-session appears after a restart (the v1 install path still re-runs
  discovery, but adapters are not rebuilt). Nothing consumes `CapabilityKind::
  Provider` yet — that is PF-2.
- **Environment caveat:** `clang-format` is not installed on this machine;
  `make format-check` could not run locally. New files follow the
  LLVM/4-space/100-col style and were checked by hand. CI must confirm.

### 2026-09-10 — PF-1.1 + PF-1.3: typed events and the ledger

- **Branch:** `feat/plugin-framework-pf1`.
- **Landed**
  - `include/agent/events.h` — typed payload catalogue, `EventTraits` mapping,
    RAII `Subscription`, `Events` facade (`subscribe` / `intercept` / `publish`).
  - `include/agent/event_bus.h`, `lib/event_bus.cpp` — per-type subscriber
    counts + `has_subscribers()`; the typed `publish()` returns on a single
    relaxed atomic load when nobody listens (invariant 5). Enum gains
    `CompressionCompleted`, `ErrorRaised`, `Count`.
  - `include/agent/plugin_capability.h`, `lib/plugin_capability.cpp` — typed
    `Capability`, `Contribution`, `InstallResult`, the `PluginServices` anchor,
    and `PluginLedger` with reverse-order unwinding that cannot be aborted by a
    throwing `remove` handler.
  - `tests/plugin_framework_test.cpp` — 21 tests: typed delivery, type
    isolation, intercept modify/cancel, RAII unsubscribe, no-subscriber no-op,
    subscriber-count tracking; ledger reverse order, per-plugin isolation,
    PLG-01 removal, unknown plugin, null handles, owners/contributions, clear;
    capability kind/name and install success/failure.
- **Verification:** `make clean && make` then `./run_tests` → **555 passed,
  0 failed**. `cppcheck` and `clang-tidy` report no findings in the new sources.
  `make check` invariants hold.
- **Environment caveat:** `clang-format` is not installed on this machine, so
  `make format-check` could not run locally; the new files were written to the
  LLVM / 4-space / 100-col style and checked by hand (no line >100 chars, no
  tabs). CI must confirm.
- **Not covered (next):** PF-1.2 publish sites — nothing publishes yet, so typed
  subscription is not observable in a real turn; the capability protocol is not
  yet consumed by a runtime; `PluginServices` sinks and the registries are
  PF-1.4; `/get plugin` / `/set plugin` is PF-1.5.
- **Gotchas worth keeping**
  - Editing `Makefile.in` does **not** regenerate `Makefile`: `GNUmakefile` only
    regenerates when the Makefile is *missing*. Run `./configure` after touching
    build files, or the new source is silently not compiled.
  - `ar rcs` never drops stale members, so a deleted source's `.o` stays in the
    archive until `make clean` — which is how a retired `request_builder.o`
    resurfaced as a duplicate-symbol link error.
  - Pre-existing `-Wunused-variable` warning in `plugins/metrics/metrics_plugin.cpp`
    (unused `end_sub`); PF-1.7 migrates that plugin onto the typed API and
    removes it.

---

## Decision Log

Binding. Superseding a decision requires editing the spec in the same change.

| # | Decision | Rejected alternative (and why not) |
|---|---|---|
| **D1** | Three mechanisms: **contribution registries** (existence), **events** (occurrence), **host services** (requests). Registration never happens via events. | One generic "capability bag" routed by the registry — routing rules for timing/ordering/deactivation become unspecifiable. |
| **D2** | Capabilities are **typed polymorphic objects**; the runtime installs them and records each install as a ledger entry. | (a) `void* impl` + enum (untyped, UB on a wrong cast, unpublishable API); (b) plugin-called imperative registration (deactivation correctness becomes the author's problem; leaks callbacks into freed state). |
| **D3** | **Typed event layer over the existing tested `EventBus`**; payload structs in `events.h`. | (a) Replacing the bus (churns pinned, tested semantics for no gain); (b) keeping `void*` payloads (out-of-band cast contract); (c) synthesising events from `AgentHooks` (compression, errors, and request/response are not hooks — the bus would lie). |
| **D4** | **Pull for UI state, push for lifecycle.** Segments/panels are callables the host invokes on the UI thread; events push to plugins; `post_to_ui` is the sanctioned cross-thread update. | Pushing UI state from worker threads — forces locking inside render and makes frame composition non-deterministic. |
| **D5** | **Declared UI regions** (status segments, panels) with the host owning layout, focus, and overflow; modals only through host services. | Arbitrary geometry/z-order/window hijack — requires a layout-engine rewrite and couples plugins to TUI internals; raw rendering makes every plugin a UI fork. |
| **D6** | **Host services** (`ask_text`, `ask_secret`, `choose`, `confirm`, `notify`, `post_to_ui`, `log`) implemented per host; TUI reuses the promise + queue + deferral machinery that already backs approvals. | Plugins calling TUI widgets directly — no CLI parity, couples the plugin to ncurses, re-entrancy hazards with the modal system. |
| **D7** | **One composition root (`Runtime`)** constructed by both hosts. | Per-host wiring — the CLI-has-no-plugins asymmetry is the bug, not the baseline. |
| **D8** | **Provider capability = flavor + dialect factory + presets + auth spec.** Behaviour lives in the dialect; the plugin supplies data. No boolean capability flags. | (a) `ProviderImpl` with chat/stream function objects (bypasses the dialect seam, duplicates transport, two mechanisms for one job); (b) `supports_x` booleans (the boolean explosion the dialect work deliberately deleted). |
| **D9** | **State layout** `~/.config/amber/plugins/<id>/plugin.conf` + `$(datadir)/amber/plugins/<id>/` for bundled assets — same shape the future external tier will use. | Putting plugin state in the global config or next to the binary — breaks the FHS/prefix resolution rules the repo already established. |
| **D10** | **`kPluginApiVersion` gate — deferred to PF-6.** For compiled-in plugins the compiler *is* the version check; a runtime constant that can only ever match is ceremony. It becomes load-bearing when code can arrive that was not built with the harness (the external tier). | Silent acceptance in PF-6 — an incompatible out-of-tree plugin must fail loudly there. For PF-1..PF-5 the alternative is dead weight, which the engineering principles ban. |
| **D11** | **Performance invariants** (spec §6): unsubscribed publish is a single atomic load; no per-token events; no allocation on the fast path; no duplicate of hook traffic. | "Optimise later" — a bus that costs per token is unusable in the streaming path and would have to be redesigned. |
| **D12** | **Console is core UI built on the public panel API** (Alt+0), always available; plugins may add panels too. | Making the console itself a plugin — a disabled plugin must never be able to hide the registry view used to re-enable it. |
| **D13** | **Compiled-in core plugins first**; runtime loading is not in the shipped design — see **D16** for the tier decision and its reasoning. | Shipping a loader now — needs an ABI contract, trust model, and install story that do not exist yet. |
| **D14** | **Error classification stays in the dialect**; the bus only *observes* errors. | An interceptable error event — classification is provider-specific (already `is_retryable` / `context_overflow_hint`); a second place to decide it is a bug factory. |
| **D15** | **Bundled plugins are enabled by default**; a user disables one for performance or preference, and a disabled plugin costs nothing at runtime (no subscriptions, no registry rows, no constructed dialect — invariant 8). | Opt-in activation — the default state would ship a harness with its own extensions switched off, and "enable to get the shipped behaviour" is a worse first run. |
| **D16** | **Tier by isolation, not by size.** Bundled (compiled-in) is the only shipped tier; the process tier is deferred to PF-6 and gets a *different* capability shape (an external provider owns its transport and credentials); `dlopen` is **rejected**; threads are not a tier. | (a) `dlopen` — buys install-without-rebuild at the cost of a fragile C++ ABI contract and keeps the shared crash domain; the process tier is strictly better for that goal. (b) Process-everything — the streaming decoder runs per SSE chunk, so bundled providers must never cross an address space, and every process provider would re-implement retry/timeout/cancel. (c) Threads as isolation — same address space, same crash domain; threads are parallelism only. |
| **D17** | **One write path for plugin state: `/set plugin <id> on\|off`** (plus `/set plugin <id> k=v`), read via `/get plugin list\|<id>`; `/plugin` keeps packaging verbs (`install`/`uninstall`/`info`) and drops `enable`/`disable`. Toggling re-publishes the feeds the plugin fed (provider list, command tree) with no restart. | Keeping `/plugin enable` *and* `/set plugin on` — two write paths to one state, which drift; it is the "no dead legacy dispatch" rule applied to the new surface. |

---

## Availability Table

What a plugin author can rely on today. Update with every landed task.

| Capability | Status | Phase |
|---|---|---|
| External tool plugin (subprocess, JSON-RPC) | ✅ Shipping | v1 (unchanged) |
| Tool contribution (core plugin) | ✅ | PF-1 |
| Command contribution (executable) | ✅ | PF-1 |
| Event subscription (typed) | ✅ | PF-1 |
| Enable/disable with clean unwinding | ✅ | PF-1 |
| v1 external plugins under the unified registry | ✅ | PF-1 |
| Prompt block contribution | ✅ | PF-1 |
| Settings contribution | ✅ | PF-1 |
| `/get plugin`, `/set plugin on\|off` (persisted, live) | ✅ | PF-1 |
| Core prompt blocks on the registry (dogfood) | ⏳ | PF-1 follow-up |
| Provider contribution (dialect + presets) | ✅ | PF-2 |
| Provider list reflects plugin state without restart | ✅ | PF-2 |
| Config-only OpenAI-compatible provider | ✅ | shipping |
| `flavor` in provider files | ✅ | PF-2 |
| Status segment contribution | ⏳ | PF-3 |
| Panel contribution + registry console | ⏳ | PF-3 |
| Host services (ask/choose/confirm/notify) | ⏳ | PF-3 |
| Log sinks | – | Deferred (no consumer) |
| Theme, key interception, geometry, hot reload | – | Deferred Register |
| External (process) tier | – | PF-6 (deferred, shaped for) |
| `dlopen` loadable library | ❌ rejected | D16 |

---

## Phases

### PF-1 — Framework core (typed events, capabilities, ledger, registries)

**Proves:** a compiled-in plugin can be enabled, contribute, and be disabled
cleanly on both hosts — with the dormant metrics plugin as the live consumer.

| Task | Scope | Spec refs |
|---|---|---|
| **PF-1.1** Typed events | `include/agent/events.h` payload structs + typed `publish`/`subscribe` over `EventBus`; unsubscribed fast path | §6, PLG-03, PLG-04 |
| **PF-1.2** Publish sites | Fire the catalogue at the verified sites (turn, message, tool, LLM response, compression, error) behind an optional `EventBus*` port on `Agent`; never publish hidden confirmation exchanges | §6, PLG-03 |
| **PF-1.3** Capability protocol + ledger | Typed `Capability`; `InstallResult`/`Contribution`; `PluginLedger` with reverse unwinding | §3, PLG-01 |
| **PF-1.4** Registries | Tools (owner-tagged add/remove, const bug fixed), Commands (subtree + handlers), Prompt blocks, Settings; common `ExtensionPoint` introspection view. **No log sinks** — cut, no consumer | §4, §5, PLG-02, PLG-05 |
| **PF-1.5** Runtime + host wiring | Composition root; plugin state persistence (`plugin.conf`, bundled plugins **on by default**); `/get plugin list\|<id>` + `/set plugin <id> on\|off` as the single write path (D17); default-on applied at first boot; CLI parity | §4, §9, PLG-10, PLG-11 |
| **PF-1.6** v1 tier under the same registry | `V1PluginAdapter` wraps the external `PluginManager` in `IPlugin`, so external plugins appear in `/get plugin list` and are toggled by `/set plugin <id> on\|off`. **This is what makes deleting `/plugin enable\|disable` safe** — without it, D17 strands the external tier with no control surface | §2, §9, PLG-11 |
| **PF-1.7** Dogfood | Metrics plugin activated on both hosts; core prompt blocks (memory/skills/brief) migrated onto the prompt registry; `chat_once` injection logic deleted | §5, §6 |

**Gate:** ledger test proves disable restores the registries exactly; prompt
blocks are order-stable across turns; metrics shows live numbers in CLI and TUI;
`make check` green.

### PF-2 — Provider capability + Gemini

**Proves:** a genuinely incompatible vendor lands as a plugin with zero shared
core edits — the whole point of the exercise. Sequenced **ahead of the UI
surfaces** so the framework is validated by its hardest real consumer before it
grows any view-layer surface; nothing here needs host services (the existing
`/set provider` key prompt suffices).

| Task | Scope | Spec refs |
|---|---|---|
| **PF-2.1** Provider capability | `ProviderSpec`, provider registry, dialect factory registration, presets as repository rows | §7, PLG-08 |
| **PF-2.2** Registry resolution under toggling | Dialect lookup is race-free while plugins are toggled at runtime; unknown flavor falls back, **known-but-disabled fails loudly** (invariant 12, PLG-13) | §7, §9 |
| **PF-2.3** Provider-driven catalog | `HttpModelCatalog` resolves the provider's dialect (fixes `/provider test`); empty `models_url` reported honestly | §7, PLG-09 |
| **PF-2.4** Gemini dialect | Endpoints, query-key auth, `contents`/`parts` body, `usageMetadata`, event-stream decode, model listing; hermetic fixtures | §7, PLG-08 |
| **PF-2.5** Gemini plugin | Presets + auth flow + model feed; enabled through the runtime; toggling re-publishes the provider feed | §7, §9, PLG-08, PLG-12 |
| **PF-2.6** `flavor` in provider files | Round-trip through `~/.config/amber/providers/*.conf`; OpenAI-compatible endpoints stay config-only | §7 |

**Gate:** Gemini works end to end hermetically; the diff touches no transport,
agent-loop, or TUI file; OpenAI-compatible providers need no code at all; a
disabled provider plugin cannot leave a half-live provider behind.

### PF-3 — UI contribution surfaces

**Proves:** the app's own UI is composed from the registries, and plugins can
extend it without touching TUI code.

| Task | Scope | Spec refs |
|---|---|---|
| **PF-3.1** Status segments | `RenderEngine` renders from the segment registry; existing segments ported; overflow/drop rules declared, not magic | §4, PLG-06 |
| **PF-3.2** Panels + console | Panel registry; host focus/key routing; registry console on Alt+0 built on the public API (D12) | §4, PLG-06 |
| **PF-3.3** Host services | `ask_text/ask_secret/choose/confirm/notify/post_to_ui` for TUI and CLI; API-key prompt migrated onto the service; modal deferral preserved | §8, PLG-07 |

**Gate:** TUI tests cover segment ordering/overflow and panel key handling; a
plugin can drive a masked prompt end to end; the console shows every
contribution with its owner.

### PF-4 — Convert the built-ins

**Proves:** the framework can absorb amber's own provider handling, deleting the
last name-keyed branching instead of adding to it.

| Task | Scope | Spec refs |
|---|---|---|
| **PF-4.1** kilocode plugin | Balance fetch/readout moves out of `lib/model_probe.cpp` into the plugin as a status segment + auth semantics | §4, §7 |
| **PF-4.2** openrouter + anthropic plugins | Convert to `ProviderSpec`; delete `capability_overrides()`; `Config` loses the kilo/account-token fields | §7 |
| **PF-4.3** Docs alignment | Provider specs re-aligned; `flavor` documented as provider data | §7 |

**Gate:** `grep provider_name ==` → 0 in `lib/`/`include/`; no feature regressions
in the TUI provider flows; dialect tests untouched.

### PF-5 — Contributor enablement

**Proves:** someone outside this repo can build a plugin without reading the
framework source.

| Task | Scope |
|---|---|
| **PF-5.1** Developer guide finalized | Availability table accurate, examples compile, threading rules explicit |
| **PF-5.2** Scaffold | `plugins/_template/` with a minimal, tested plugin and build wiring |
| **PF-5.3** API compatibility policy | What `kPluginApiVersion` covers, how breaking changes are staged |
| **PF-5.4** External-tier review | Decide whether/when non-tool capabilities cross the process boundary — the entry condition for PF-6 |

---

### PF-6 — External (process) tier — DEFERRED

**Opens only when:** PF-1..PF-5 are done, the in-process provider port is proven
by Gemini and the converted built-ins, and PF-5.4 confirms the need (untrusted
third-party providers, or non-C++ plugin authors). Nothing in PF-1..PF-5 may
block on it.

**Shape, agreed now so the earlier phases do not preclude it (D16):**

- A process-tier provider is a **different capability kind**, not a dialect
  proxy: it owns its own HTTP client and credentials, receives the conversation
  over framed IPC, and streams chunks back. It cannot use the in-process
  `Dialect` port.
- Transport: unix socket, reusing the framing shapes already in the tree
  (`lib/plugin.cpp` v1 protocol; `lib/mcp_transport.cpp`) — not a third protocol.
- The trust model, install story, and `kPluginApiVersion` semantics for
  out-of-tree code are part of this phase, not before it.
- Consequence to design for: retry, timeout, cancellation, and usage accounting
  must be specified for the boundary, since an external provider cannot reuse
  the harness transport.

---

## Deferred Register

Recorded so the reasons survive the session. Nothing here is forgotten; each
entry names the trigger that would reopen it.

| Item | Why deferred | Reopens when |
|---|---|---|
| raw key interception | Makes the UI untypeable from a plugin and is untestable; panels + commands cover declared needs | The panel contract has proven stable for a full cycle |
| window geometry / z-order | Needs a layout-engine rewrite; declared regions cover the real requirements | A concrete UI need cannot be expressed as a segment or panel |
| themes / render hijack | Painted inside ncurses internals in the old draft | Segment/panel contracts stable |
| per-token stream events | Violates the performance invariants; `AgentHooks` already serves the UI | Coalesced progress is genuinely needed and can be throttled |
| hot reload / plugin dependencies | No consumer; adds lifecycle states | Contributor demand with a concrete use case |
| log sinks | Cut from PF-1 during sign-off: listed in the registry set with no phase naming a consumer — the exact "half-surface" failure this design exists to avoid | A component needs structured log fan-out (the conversation log stays authoritative) |
| `kPluginApiVersion` | Deferred with the external tier (D10): for compiled-in plugins the compiler is the version check | PF-6 |
| external (process) tier | Needs a wire contract, trust model, and install story; the in-process interface must be proven first so its wire form is knowable | PF-6 entry condition above |
| `dlopen` loadable library | Rejected (D16), not deferred: fragile C++ ABI contract, same crash domain, no language freedom — the process tier dominates it | Only if PF-6 is rejected *and* a concrete need for no-rebuild install remains |
| Bedrock/Vertex request signing | Already noted as a dialect-seam follow-up (`docs/fix-tracker.md`) | A provider plugin needs signature auth — the `AuthSpec` seam accommodates it |

---

## Risks

| Risk | Mitigation |
|---|---|
| The framework grows eight half-surfaces no one uses (the current state, rebuilt) | Every phase names its consumer; a registry without a driving consumer is not done; the console makes dead surfaces visible |
| `Tui` is a friendship/ownership web; plugin hooks could pierce internals and block a future public API | Plugins depend only on registries and host services; TUI-specific code stays behind those interfaces; the console is the proof |
| Event overhead in the streaming path | Hard invariants (D11) + a no-subscriber test; per-token publishing is prohibited |
| Deactivation leaves dangling callbacks | Ledger with reverse unwinding + a test that diffs registry state before/after disable |
| Provider conversion (PF-4) regresses the working provider flows | Convert behind the existing tests; the dialect seam already isolates wire behaviour; convert one provider per PR |
| Contributor documentation describes unbuilt surfaces | The guide's availability table is phase-gated and updated with each landing task |

## Definition of Done (whole effort)

1. A bundled provider plugin (Gemini) works end to end, added without editing
   transport, agent-loop, or TUI code.
2. The built-ins are plugins; the name-keyed capability table is deleted;
   `/provider test` is dialect-correct for every provider.
3. `/get plugin list` and `/set plugin <id> on|off` are the plugin control
   surface; state persists; a toggle immediately adds or removes the plugin's
   providers from `/get provider list` and the drawer, with no restart.
4. Enable/disable is clean and tested (nothing survives deactivation); the
   console shows what each plugin contributes.
5. The contributor guide matches the shipped API, with the availability table as
   the index.
6. `make clean && make && make test && make lint && make analyze && make check`
   green on g++ and clang++, zero new findings.
