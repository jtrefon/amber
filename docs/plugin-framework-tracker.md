# amber, Plugin Framework Tracker

- **Status:** 🟢 PF-1, PF-2, PF-3.1, PF-3.2 and PF-4 complete; 713 tests green.
  The framework runs end to end: **every** provider amber ships, custom,
  openrouter, kilocode, anthropic, gemini, opencode_go, opencode_zen,
  commandcode, deepseek, is a plugin, the core declares none, the status bar
  and the panel view are composed from registries, and switching a provider
  plugin off removes it live. Open **by decision, not by omission**: the core
  prompt-block migration (blocked on a behaviour question with a bench harness
  waiting) and host services (no consumer yet).
- **Direction (stated 2026-09-10):** the long-term target is a microkernel,
  amber as orchestrator + plugin registry, with *everything else* (not just
  providers) arriving as a plugin. The phases below are the path there, and no
  phase may introduce core domain state that a later extraction would have to
  unpick.
- **This tracker is the engineering log**, not the contributor contract. The
  **single status surface for contributors is the availability table in
  `docs/spec/plugins/developer-guide.md`**: when a task lands, update that
  table in the same commit.
- **Reference:** `docs/spec/plugins/plugin-framework.md` (the design record)
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
   its checkbox and the availability table in
   `docs/spec/plugins/developer-guide.md` in one commit with the code.
3. **Decisions are recorded, not re-litigated.** The decision log (D-series) is
   binding: a change to a decision is a change to the spec, and it is made here
   first with the reason and the superseded alternative named.
4. Verification before any task is marked done:
   `make clean && make && make test && make lint && make analyze && make check`
   (g++ and clang++), zero new clang-tidy/cppcheck findings.
5. **No task ships its capability half-wired.** A registry that exists but is not
   driven by a real consumer is not done, every phase names its consumer.

## Legend

```
[ ], Not started   [~], In progress   [x], Done, all checks pass
[!], Blocked       [–], Deferred (see Deferred Register)
```

---

## Baseline, pre-implementation snapshot (2026-09-10, commit `c1ad6a8`)

> **Historical record, not current state.** This table captures the tree
> *before* the framework was built; it is why the rebuild happened, not what is
> true today. Several rows are now **false**: activation *does* happen, the CLI
> *is* wired, and `Capability` *is* read in production. For current status, use
> the availability table in `docs/spec/plugins/developer-guide.md`.

Evidence captured before designing; this is the baseline the framework was
measured against. Re-verify rather than trust it if the tree has moved.

| Area | State | Evidence |
|---|---|---|
| `EventBus` primitive | Implemented, well tested, **inert in production** | `lib/event_bus.cpp`; only tests and the dormant metrics plugin fire it |
| `PluginRegistry` lifecycle | Implemented, tested | `lib/plugin_registry.cpp:8-65`; `tests/plugin_core_test.cpp` |
| Plugin activation | **Never happens outside tests** | `tui/tui_main.cpp:158`, `tui/tui.cpp:96-98` construct + set context only |
| `Capability` | Declared, **never read in production**; `void* impl` | `include/agent/plugin_core.h:17-33,53` |
| Tool contribution | **Impossible by design** | `PluginContext::tools` is `const` (`plugin_core.h:37`); `register_tool` is non-const (`registry.h:29`) |
| Plugin commands | Render in the drawer, **cannot execute** | `tui/tui_input.cpp:1253-1261` (no `plugin.*` handler registered) |
| Status bar | Hardcoded segment list, private `Seg`, magic drop priorities | `tui/render_engine.cpp:160-243,305-439`; `tui/render_engine.h:76-80` |
| Key handling | Hardcoded `switch`, no keymap | `tui/tui.cpp:557-607` |
| Windows | Single full-screen layout, one active window, every window spawns an `Agent` | `tui/render_engine.cpp:25-26`; `tui/window_manager.cpp:16-38` |
| Host-mediated input | Works for approvals/API keys via promise + queue + modal deferral | `tui/event_router.cpp:135-150,217-224,267-282`; `tui/tui_input.cpp:1981-2018` |
| Prompt assembly | Four ad-hoc injection algorithms in one function | `lib/agent.cpp:190-204,244-263,290-319,327-335` |
| v1 external plugins | **Working** (subprocess JSON-RPC, tools only) | `lib/plugin.cpp:154-193,426-479`; `tools/plugins/{sysinfo,cdp}` |
| Provider dialect seam | Working; `register_dialect` already public | `include/agent/dialect.h:89`; `lib/dialect.cpp:14-34` |
| Provider capability routing | Name-keyed table, no plugin path | `lib/providers_service.cpp:12-33` |
| `/provider test` for non-OpenAI | **Broken**: probes with the default dialect | `lib/providers_catalog_http.cpp:14-24` |
| CLI plugin support | None | `src/main.cpp` has no plugin wiring |

---

## Progress Log

Newest first. Each entry: what landed, on which branch, and what it did *not*

### 2026-09-12 — One wallet: the duplication finding (3) resolved

Branch `refactor/one-wallet`, stacked on the toolset audit.

- **Decision: `wallet` is the surviving word.** It is the surface users already
  type (`/get provider wallet`) and read (`$13.22`), it is already the config
  key, and "allowance" names only one of the shapes (quota windows) rather than
  the question both answer. The mechanism is not named differently from the
  surface it backs (D9).
- **One of each:** `WalletRegistry`, `WalletCapability`, `CapabilityKind::Wallet`,
  `WalletSnapshot`, one config flag, one status segment, one command pair.
  `AllowanceRegistry`, `AllowanceCapability`, `AllowanceSnapshot`,
  `AllowanceWindow`, `CapabilityKind::Allowance`, `allowance_enabled` and the
  `/get|/set provider allowance` nodes are deleted, not deprecated.
- **The richer snapshot is the payload** — `WalletSnapshot{plan, windows[],
  credits_balance, unit, currency}` — so nothing the allowance half could report
  was lost; the wallet's old bare `optional<double>` becomes
  `WalletSnapshot::of_balance(amount)`, which is what the three balance-only
  plugins (kilocode, openrouter, deepseek) now return.
- **The bar rule, in one place:** the balance when the snapshot has one,
  otherwise the window closest to reset. Width is the constraint; `/get provider
  wallet` carries the whole picture.
- **Deleted with it:** the duplicated polling machinery (two state structs, two
  ticket counters, two dirty flags, two schedulers, two turn subscriptions, two
  refresh floors) and four tests that duplicated the wallet's own — the registry
  tests and the unwind test, which now cover one registry instead of two.
- **A defect the duplication hid:** `allowance_enabled` was never parsed or
  saved (`lib/config.cpp` had no `allowance` key and `save_global` never wrote
  it), so `/set provider allowance off` reported success and forgot it. The
  provider-switch path also refreshed only the wallet, leaving the allowance
  stale until the next turn ended. One mechanism cannot have half a control
  surface; both are gone with it.
- **Evidence:** 729 tests green (was 732: −4 duplicated, +1 new for the
  precedence rule). Live probe against the built libraries: **one** `wallet`
  status segment and zero `allowance` segments, with all five provider plugins
  (commandcode, deepseek, kilocode, opencode_go, openrouter) declaring the same
  `Wallet` capability.
- **Found while here, fixed:** the contributor guide documented a superseded
  panel API (`class ConsolePanel : public PanelCapability` with `render`/
  `handle_key` overrides) that could not compile — `PanelCapability` takes a
  `PanelSpec`. The stale example is deleted; the accurate one stands.


### 2026-09-12 — The toolset audit (§5.2): roles as capability data

Branch `refactor/toolset-audit`, stacked on the flag retirement.

- `ToolMeta` gains a `role` beside its `verb`, and `ToolCapability` declares one
  `Meta` record per tool instead of a bare verb map — so what a tool *is* and
  what it *does* are stated once, together, on the capability.
- `audit_toolset` (`lib/toolset_audit.cpp`) reports two shapes: a **deficiency**
  (a required role no enabled tool fills — required is `{Read}`) and an
  **ambiguity** (two tools in one role whose descriptions overlap by Jaccard
  ≥ 0.6 on significant words, so the model cannot tell them apart).
- `PluginRuntime::audit()` computes it on demand, never cached, so it cannot
  describe a configuration that has moved on. The TUI reports the findings after
  every enable/disable, next to the line that says what the toggle removed: that
  line says what left, the audit says what it costs.
- A warning, never a gate (D24): a hard lock would forbid the
  two-search-implementations comparison this domain exists to enable.
- **Evidence:** 732 tests; `audit_the_shipped_tool_set_is_clean` is the
  acceptance test — the set amber ships must not trip its own audit, and every
  shipped tool declares a role (one that does not is invisible to the audit).
  `runtime_audit_follows_the_enabled_toolset` proves the live path: disabling
  `tool_read` produces exactly one deficiency, re-enabling clears it. Hermetic
  bench unchanged at 8/8.


### 2026-09-12 — The tool flags retire; plugin state is the only control (§5.3)

Branch `refactor/retire-tool-flags`, stacked on the tools-domain work.

- `Config::plan_tool` / `Config::task_tool`, their `AMBER_*` env vars and the
  config-file keys are deleted, along with the two trailing `bool` parameters of
  `register_default_tools`. `plugins/tool_plan` and `plugins/tool_task` no
  longer read config: presence is plugin state, switched with
  `/set plugin tool_plan off`.
- `prompts/tools_planning.md` becomes `prompts/tools/plan.md`, contributed by
  the plan tool as its own `System` block (`tool_doc_priority::kPlan`). The
  prose now travels with the tool, which is what the whole split was for.
- **One deliberate behaviour change:** the plan and task tools ship *enabled*,
  like every other bundled tool plugin (D15). Before, both were off unless a
  flag was set. A user who wants the old shape switches the plugin off.
- The bench's per-scenario `task_tool` opt-in is gone; a scenario that scripts
  sub-agent replies is a scenario that delegates, and that is now the only
  signal it needs.
- **Evidence:** hermetic scorecard unchanged at 871.25/1000, 8/8 scenarios
  (`bench/results/tools-domain-flags-retired.txt` vs the baseline). 723 tests.


### 2026-09-12, The bench runs the plugin runtime (tools spec §3.6)

Branch `docs/tools-domain`. The harness now measures what a user runs, and a
result says what produced it.

- `bench/runner.cpp` builds the runtime every host builds (`add_bundled`,
  attach host services + config, `start`) instead of calling
  `register_default_tools` directly. That also restored the tool documentation
  the tool split had silently removed from bench prompts.
- `RunMeta` carries the plugin set; serialized, parsed back (the round-trip
  rule in `bench/report.cpp`), and printed in the text and scorecard headers.
  Older results say "plugins: not recorded" rather than implying a default.
- `--disable ID[,ID]` switches plugins off for one run. An unknown id fails
  loudly, and the failure names the scenario and the reason.
- **Two invariants keep a run independent of other people's state:**
  `PluginRuntime::apply_state` applies without persisting (a measurement is not
  a preference, so no bench run writes the user's `plugin.conf`), and
  `PluginRuntime::start(false)` ignores persisted state entirely. The second was
  found the hard way: this machine's saved state had `anthropic` and
  `tool_search` off, and the first bench runs reported a configuration nobody
  chose.
- Baseline artifact: `bench/results/tools-domain-hermetic-baseline.{txt,json}`,
  8/8 hermetic scenarios, 17 plugins, all on. This is the "before" for the
  flag retirement (§5.3), whose prompt change (the plan tool's documentation
  moves into its plugin) needs the comparison.

724 tests pass, `make check` clean.


### 2026-09-12, One plugin per tool, and the prompt travels with the tool

Branch `docs/tools-domain`. The tools domain now has the granularity the spec
asked for, and the model-facing documentation can no longer disagree with the
tool schema.

- **Split (§3.1):** `tool_search`, `tool_read`, `tool_write`, `tool_bash`,
  `tool_process`, `tool_plan`, `tool_task`; `plugins/core_tools` deleted.
  `register_default_tools` installs the same per-tool definitions for hosts
  with no runtime, and now takes the prompt registry so a tool and its prose
  arrive together on both paths.
- **Prompt blocks gained a placement (§3.4):** `System` (the stable prefix,
  where tool documentation belongs), `Head`, `Tail` (default, so no existing
  contributor moved). Without this, per-tool documentation would have landed
  *after the conversation*, a behaviour change on every request, and the wrong
  place for capability instructions.
- **The system prompt is no longer sealed (staleness fix):** it is re-derived
  per turn and rebuilt only when the text changed, so a plugin toggle changes
  schema and prose in the same action. A prior external review flagged this
  class of bug ("the model's tool documentation can become stale"); it is now
  closed for tools and covered by `disabling_a_tool_plugin_removes_its_documentation`.
- **Tested invariants:** the assembled prompt is byte-identical to the pre-split
  text (`tests/fixtures/tools_prompt_at_migration.md`), section order included;
  disabling `tool_search` removes exactly its section and its schema.
- **Deliberately not done, in the spec's §8:** the `plan_tool`/`task_tool` flag
  deletion (needs §3.6 first, or the bench loses its per-scenario switch), the
  skill tools (need catalog ownership to move out of `Agent`), the bench plugin
  state (§3.6) and the audit (§5.2).

Suite 722 passed, `make check` clean.

cover.

### 2026-09-12, Command capability: plugins can register slash namespaces

The command contribution capability was removed earlier for having no producer
and no consumer. A producer now exists (the bundled `plugins/hello/` example),
so it is re-added, with a consumer that did not exist before: the TUI binds
plugin commands into the slash engine.

- **`CommandCapability` + `CommandRegistry`** (`lib/extensions.cpp`,
  `include/agent/extensions.h`). `CommandSpec{root, help, man, subtree,
  handlers}`; ledgered like every other contribution, so disable unwinds it.
- **Host binding** (`tui/slash_dispatcher.cpp`). `refresh_completions()` merges
  each contribution as `{root: {help, man, children: subtree}}` and stamps each
  executable leaf with a derived action (`plugin.<id>.<path>`). The bound action
  resolves the handler from the live registry at dispatch time, so a
  disable/re-enable can never dispatch a stale closure.
- **Immediate toggle** (`tui/tui_input.cpp`). `/set plugin <id> on|off` now
  rebuilds the command tree, so a plugin's commands appear or disappear with no
  restart.
- **Worked example** (`plugins/hello/`). `/hello greet amber` returns
  `Hello, amber!`. Handlers return the text to display, so the plugin stays
  UI-free.
- **Tests**: `runtime_command_capability_installs_and_unwinds`,
  `runtime_command_contribution_is_listed_and_removed`,
  `bundled_hello_plugin_contributes_the_hello_command`,
  `test_plugin_command_subtree_merges_and_resolves`.
- **Not covered:** the headless CLI has no command tree, so a command
  contribution is TUI-only. Per-plugin settings remain removed.

### 2026-09-11, Fix: tool ownership is recorded, so unwinding cannot cross plugins

Branch `feat/capability-path`. Found by reviewing the tools-as-plugins work
before landing it (the stack had been pushed but not PR'd).

- **The defect.** `ToolCapability` unwound with `ToolRegistry::remove_tool(name)`,
  which is name-based, against a registry that recorded no owner, while
  `register_tool` is idempotent *by name* and **replaces** the earlier instance.
  So two plugins contributing the same tool name (the override case the
  microkernel exists to enable, e.g. a plugin replacing `bash`) meant that
  disabling one plugin deleted the **other's** tool. The ledger's contract,
  "removes exactly what this plugin added, and nothing else", was false.
- **Why it was not just a doc-level rule.** The entry below already accepted
  *name* collisions as a policy matter. This is different: it is a mechanism
  failure. The ledger can only be trusted if unwinding is identity-scoped.
- **Red → green.** Two tests, both failing before the fix
  (`./run_tests` → 694 passed, 2 failed):
  - `tool_capability_unwind_cannot_remove_another_plugins_tool` (unit: the
    capability + registry mechanism);
  - `runtime_disable_cannot_remove_another_plugins_tool` (runtime: the same
    through `set_state`, which is how a user hits it).
- **Fix.** The registry now records **who** contributed each tool:
  - `ToolRegistry::register_tool(tool, owner = {})`, `owner` is the plugin id;
    empty means host. The default keeps every existing call site unchanged.
  - `ToolRegistry::remove_owned_tool(name, owner)`, removes only when the
    recorded owner matches. This is the ledger's unwinder, so unwinding is
    identity-scoped by construction.
  - `ToolCapability::install` registers with `services.owner()` and its
    contribution removes its own names, owner-checked. A tool whose name was
    taken over by another plugin stays with its new owner.
  - `remove_tool(name)` stays as the host's escape hatch; `remove_tool` is no
    longer what the ledger uses.
- **Rejected: a whole-owner sweep** (`remove_tools_by_owner`). A capability
  whose owner was never set (empty string) would have removed every
  host-registered tool. Per-name, owner-checked removal is both precise and safe
  against that footgun.
- **Verification:** `./run_tests` → **696 passed, 0 failed**; `make check` →
  all invariants hold; clang-tidy clean on the touched sources.
- **Open finding (recorded, not fixed):** *shadowing has no restore.* If a
  plugin replaces a host tool of the same name, disabling that plugin removes
  the tool entirely, the host's displaced instance is gone, because
  `register_tool` replaces rather than stacks. No in-tree plugin does this, so
  it is latent. Reopen when a plugin actually overrides a core tool: the fix is
  a per-name shadow stack (displace on register, restore on owned removal).
- **Not covered:** the tool set is still not namespaced (`read`, not
  `core.read`), renaming would break every prompt and every user's muscle
  memory. That remains a doc-level policy; what changed is that a collision can
  no longer make unwinding reach across plugins.

### 2026-09-11, PF-4.4: the core tool set becomes a plugin

Branch `feat/tools-as-plugins`. `register_default_tools` is no longer a list of
seven factories in `lib/tools_default.cpp`; the set is declared as capabilities
in `plugins/core_tools` and installed through the same path a third-party tool
would use (9 tools from 7 capabilities, the process tools are one contribution
of three).

- **Host services, the third capability shape.** A capability that contributes
  data (a preset, a block) needs nothing; a provider needs a dialect factory; a
  *tool* needs the host's runtime objects at construction, bash binds to the
  job service, todowrite to the todo store, task to the sub-agent executor and
  the registry. `HostServices` carries those as pointers and is attached like
  the config (`attach_host_services`), so the runtime never owns host state.
- **A declining capability is not a failure.** `InstallResult::declined` marks
  "nothing to install". Without it, `todowrite`/`task`, absent by default,
  failed the plugin, which deactivated `core_tools` and left the agent with **no
  tools at all** on a default config. Caught in the live TUI, not by the unit
  suite: the install path was only covered per-capability, never through
  `activate()`. Two runtime-level tests now pin it.
- **Not covered:** the tool set is not namespaced (`read`, not `core.read`),
  renaming would break every prompt and every user's muscle memory, so the
  naming policy stays a doc-level rule. (A collision can no longer make
  unwinding reach across plugins, see the ownership fix above.)
- **Open:** core tools are now disableable like any plugin. Turning off
  `core_tools` is a one-command way to hand the agent an empty toolbox. Whether
  the registry should mark a contribution class as load-bearing is unresolved.
- **Also closed:** `make analyze` never scanned `src/` or `plugins/`, so every
  plugin was unanalyzed. Adding both surfaced one real defect, the dialect
  guard in `lib/llm.cpp` tested its constructor parameter *after* moving it, so
  it was unconditionally true and threw even when the caller supplied an
  explicit dialect (fixed, with a test).

### 2026-09-11, Fix: the wallet fetch raced the host config and outlived the runtime

Found by reviewing the merged PF-3.4 code (post-merge, PR #106).

- **Two defects, one detached thread.** The wallet refresh did
  `std::thread([this]{ perform_wallet_refresh(); ... }).detach()`:
  1. **Use-after-free.** The worker dereferenced the runtime. `~PluginRuntime()`
     only calls `shutdown()`, it never waits for or signals the fetch, so
     destroying the runtime (TUI quit, test scope) with a fetch in flight is UB.
  2. **Data race on live host state.** `perform_wallet_refresh()` called
     `active_config()`, which returns a reference to the host's own `Config`,
     in the TUI that is `cfg_`, mutated on the UI thread by provider switches
     and `/set model`. The worker read `provider_name` / `api_key`
     (`std::string`) concurrently with those writes.
- **Evidence, not assertion.** A probe that drives `tick()` while the host
  mutates its config, built with ThreadSanitizer *and* with
  `plugin_runtime.cpp` itself instrumented (instrumenting only the probe proves
  nothing, the archive copy is uninstrumented):
  - pre-fix: `WARNING: ThreadSanitizer: data race`, read of size 8 in
    `basic_string::empty()` inside the fetch, racing the host's write to
    `api_key`;
  - post-fix: 0 warnings, and the wallet still resolves (`ready=1`).
- **Fix: the worker owns everything it touches.**
  - `WalletState` is a `shared_ptr` holding only atomics; the worker keeps a
    reference, so it outlives the runtime safely.
  - The host thread **snapshots** what the fetch needs, a copy of the `Config`
    and a copy of the fetch callable, before the thread starts. The worker
    captures no `this`, reads no live state, and writes only atomics.
  - A per-request **ticket** (plus the provider the ticket was for) means a
    result that lands after a provider switch is ignored: the readout shows
    "not fetched yet" rather than another account's balance under the new
    provider.
  - `has_value` distinguishes "answered with an amount" from "answered with
    nothing", so a provider that declares no wallet reads as unavailable rather
    than as a balance of zero. (The branch's own test caught that one.)
- **Also fixed while here:** the state members are no longer duplicated on the
  runtime, so there is one source of wallet state.
- **Verification:** `./run_tests` → 655 passed, 0 failed; `make check` clean;
  clang-tidy clean on the touched sources; TSan clean as above. Two new tests:
  a fetch still in flight when the runtime is destroyed completes safely, and a
  result from the previous provider is not shown.

### 2026-09-11, Merged! PR #105

Squash-merged as `6b21243`, all required checks green (`lint`, `analyze`,
`build-and-test` on g++ **and** clang++, `build-and-test-macos`, `check`,
`complexity`, `format-check`). The one non-required failure,
`Code scanning AI findings`, is **infrastructure**: GitHub's Copilot autofind
agent exits with `400 The requested model is not supported`, no finding about
this code, and not a repo workflow. Worth knowing so it is not chased again.

### 2026-09-11, Plugin metadata (description + category), and a build fix

- **Landed**
  - **`IPlugin::description()` and `IPlugin::category()`**: the plugin
    vocabulary was id/version/name only, so a list could be counted but not
    scanned. Both are optional (defaults `""` and `"other"`), so existing
    plugins keep compiling, and both are shown: an undeclared category lands
    under `other` and an undeclared description prints `(no description)`, so
    an omission is visible rather than silent.
  - **`agent::plugin_category`**: core-declared names (`provider`, `tools`,
    `observability`, `ui`, `memory`, `search`, `other`) as a *vocabulary, not
    an enum*: a plugin may invent a category, and the console gives it its own
    heading rather than hiding it or folding it into `other`.
  - **The registry list is grouped** (`/get plugin list`, the console panel).
    Known categories come first in a deliberate order, unknown ones follow
    alphabetically, `other` last. Each entry is `id  state  version
    description`, with contributions on an indented second line, two lines
    rather than one over-long one, because the panel would otherwise truncate
    silently. `/get plugin <id>` additionally reports the category and tier.
  - Every bundled plugin now declares both fields.
- **Build fix found by this work (real, and a class we had been warned about):**
  bundled plugin `.d` files were generated but **never included** in
  `Makefile.in`, so a change to a header a plugin includes did not rebuild that
  plugin's object. Adding virtuals to `IPlugin` therefore produced a binary
  where some objects had the old vtable, the console segfaulted on a jump
  through a null slot. `-include $(wildcard $(BUILD_DIR)/plugins/*/*.d)` fixes
  it. This is the repo's own documented stale-`.o` gotcha; it had simply never
  bitten in the plugin directory before, because there was nothing there to
  change.
  - Related and expected: `make clean` also removes the MCP test fixtures, so
    running `./run_tests` directly after a clean reports MCP failures until
    `make test` rebuilds them. Not a bug, but it is how a clean run can look
    broken.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **631 passed, 0 failed**, `make check` clean, cppcheck clean, new files
  clang-format clean. Console output verified by probe: grouped by
  `provider` / `observability` with each plugin's one-line summary.

### 2026-09-10, PF-3.4: the wallet (framework-owned, all providers)

- **Landed**
  - **`WalletRegistry` + `WalletCapability`**: a provider supplies only
    `Fetch = optional<double>(const Config&)`; the runtime owns everything else.
    Installed through the ledger like every other capability, so disabling a
    provider plugin takes its wallet with it.
  - **Runtime policy:** refresh on turn end (the balance only moves because we
    spent something), on provider switch, and once at startup; a 10s floor so a
    fast tool loop cannot hammer the endpoint; the fetch runs off the UI thread,
    and the agent thread never does I/O. Switching to a provider without a
    wallet clears the value rather than showing the previous provider's.
  - **One core status segment** (drop priority 9, first to go on a narrow
    terminal): `$13.22`, or `-` when the active provider declares no wallet or
    the fetch failed. No words, per the space constraint.
  - **`/get provider wallet`** (state, holder, amount, or why there is none)
    and **`/set provider wallet on|off|toggle`**, persisted in the global
    config (`wallet=`).
  - **kilocode migrated onto it**, deleting its bespoke poll loop, atomic cache,
    status segment and 60-second timer. The plugin is now: a preset, a URL, a
    token rule, and one fetch lambda.
- **Design note (recorded):** the toggle is a single global switch under the
  `provider` namespace, **not** a per-provider setting. Two concepts were being
  conflated, "this provider has a wallet" (a plugin capability) versus "do I
  want it taking bar columns" (a display preference, and bar space is global).
  Per-provider slots in additively later if a real need appears; the bare form
  keeps its "all" meaning.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **627 passed, 0 failed**, `make check` clean, cppcheck clean. Probe against
  the built library: with a kilocode provider and an invalid key the fetch runs
  against the real endpoint and reports `supported=1 ready=0 failed=1` →
  segment `-`; a wallet-less provider (anthropic) also renders `-`; both at
  drop priority 9. Live TUI: `/get provider wallet` and
  `/set provider wallet off` both work.
- **W2, OpenRouter wallet.** `plugins/openrouter/` now declares one: it reads
  the *per-key* spend cap (`GET /v1/key` → `limit_remaining`), which is what a
  stored inference key can see, falling back to `limit - usage` when a gateway
  reports only those two, and reporting nothing when the key is uncapped. The
  wheel was not re-invented: a shared `http_get_with_bearer` (`lib/http_get.cpp`)
  now serves both wallets, so neither plugin hand-rolls libcurl. It is
  deliberately *not* the chat transport, that owns streaming, retries and a
  dialect.
- **Deliberately not built (recorded):** a `wallet_url` knob for the `custom`
  provider. The reasoning is the same one that cut log sinks (D22): no known
  endpoint serves one, OpenAI's public API has no balance endpoint at all, so
  the knob would be a config surface with no consumer. `custom` renders `-`,
  which is the honest answer, and a knob can be added the day someone names an
  endpoint that needs it.
- **Not covered:** account-wide OpenRouter credits (management key → host
  services, deferred, D22).

### 2026-09-10, Fix: the kilo wallet readout (TWO regressions from PF-4)

- **Bug:** after the kilocode conversion the balance readout stopped showing an
  amount. Reported from real use: "kilo $13.22 before, dash now, key is in".
- **Two independent causes, both introduced by the conversion:**

  1. **Stale config pointer.** The plugin resolved its token from a `Config*`
     cached in `initialize()`. Hosts construct and `start()` the runtime
     *before* handing over the config they actually mutate (the TUI takes its
     `Config` by value), so the pointer referenced the runtime's startup copy,
     `provider_name` at its default, `api_key` empty. `balance_token()`
     returned "" and no fetch was ever scheduled.
  2. **Wrong endpoint.** The conversion derived the balance URL from
     `api_base`. The balance API and the chat gateway live at *different* kilo
     paths, `api.kilo.ai/api/profile/balance` vs `api.kilo.ai/api/gateway`,
     so the request went to `.../api/gateway/profile/balance` and failed. The
     original core code hardcoded the right URL; deriving it looked like
     generalisation and was actually a behavioural change.

- **Fix:** (1) plugins read the config through the `PluginContext` at call
  time, and both hosts attach their config *before* activating plugins (TUI
  activation moved into the constructor; CLI attaches then starts); (2) the
  balance URL is a fixed endpoint owned by the plugin, exposed as
  `kilocode_balance_url()` so it can be pinned; (3) the config-lifetime rule is
  documented in the developer guide, and `PluginRuntime::find(id)` is public so
  a host or test can reach a plugin's own state.
- **Verification:** red first for each, `"" != "kilo-jwt"`, and
  `kilocode_balance_url()` pinned to the API path with no `/gateway`.
  Endpoints checked live: the correct URL answers **401** (exists, needs auth),
  the URL the bug produced answers **405**. Suite 622 passed.
- **Process lesson (recorded, not hidden):** the first fix's probe passed an
  *invalid* key, so it printed `kilo balance,` and I read that as "the honest
  failure state", it was masking the 404/405. A probe that cannot distinguish
  "correct but unauthorised" from "wrong URL" proves nothing about the URL.
  The pin on the endpoint is what makes this class of bug visible.

### 2026-09-11, Fix: compression discarded the injected memory block

Found by reviewing the merged PF-1.6 work: the prompt-block migration was left
open precisely because of this, and reading the assembly closely turned it from
a question into a defect.

- **The regression.** Injected blocks were assembled at four scattered sites.
  The memory block was injected *before* the compression gate, and the gate's
  rebuild does `prompt_copy.assign(...)`, replacing the whole prompt, so on
  any compressing turn the retrieved memories were **silently discarded**, while
  skill discovery, activated bodies, the brief and plugin blocks (all injected
  after) survived. Compression fires on long sessions, which is exactly where
  memories matter most.
- **It is a regression, not a design choice.** Before the immutable-Context
  rewrite (`a3c7af7`), compression ran on a list that *already contained* the
  injection (`prompt_msgs = history_; … retriever_->…; prompt_msgs = compress(prompt_msgs)`).
  The rewrite moved compression onto the sealed stack and added the `assign`,
  and nothing covered the injection. Evidence: `git log -S build_system_prompt_suffix`
  dates the injection before the rewrite; no test asserted a memory ever
  reached a request, on either turn type.
- **Live by default.** Experience is on by default (`experience.h:44`) and the
  retriever is wired in both hosts (`tui/window_manager.cpp`, `src/main.cpp`).
- **The fix, one assembly point, after the gate.** The four sites are replaced
  by `Agent::inject_prompt_blocks`, called exactly once and only after the
  rebuild, so there is no "before the gate" left to inject into by accident.
  Order is explicit in `prompt_priority` (declared beside the assembly) rather
  than reconstructed from injection positions: head blocks (memory 100,
  discovery 200) sit with the system prompt; tail blocks (activated bodies 900,
  brief 950, plugin blocks 1000) follow the conversation. The sealed `Context`
  is untouched, this is the prompt copy.
- **Why the core blocks are NOT registry entries (the dogfooding answer).**
  `PromptRegistry` is runtime-owned and shared across windows; the retriever,
  skill catalog and brief store are per-agent. Registering a core block there
  would put one window's state into another window's prompt. The registry is for
  app-wide contributions; core blocks are assembled by the agent that owns their
  state, and both are merged in one ordered pass. That reasoning is now written
  down rather than discovered again.
- **Byte-identical on ordinary turns, verified, not asserted.** A probe dumps
  the full prompt (memory + brief + plugin block, no compression) and the output
  was diffed across the change: **identical**. So the blast radius of this fix is
  compressing turns only.
- **Verification:** red first, `agent_keeps_injected_blocks_when_compression_fires`
  failed with the memory text never reaching the model, while the control
  (`agent_injects_in_memory_blocks_without_compression`) passed. Green after:
  `./run_tests` → **658 passed, 0 failed**, `make check` clean. New contract:
  `docs/spec/agent-loop/prompt-assembly.md` (invariants, PA-01..04).
- **Also fixed here (mine, from #105):** two compiler warnings in
  `lib/dialect_gemini.cpp`, a dead `text_of_parts` helper and an unused
  `stream` parameter. `-Wall` is not a CI gate, so they survived review; they
  are fixed and named in the commit rather than left as known noise.

### 2026-09-11, Hardening the plugin surface: core UI dogfood, two dead kinds pruned

Three findings from reviewing the tools-as-plugins work, closed together.

**1. Disabling a plugin said nothing.** `/set plugin <id> off` silently persisted
and unwound; the user had to run `/get plugin <id>` afterwards to learn what
disappeared. `SlashDispatcher::set_plugin` now reports the change in one place
(both the command tree and the registry land there): `plugin core_tools off
(removed 9 tool)`. Grouped by capability kind, because "9 tool" and "1 segment"
mean different things.

- *Investigated and rejected:* a warning for "no tools left, the agent cannot
  act". The premise turned out to be **false**: `Agent` always registers the
  three skill tools (`read_skill`, `list_skills`, `write_skill`,
  `register_skills=true` by default and no host overrides it), so the registry
  cannot reach zero in production and the warning would have been unreachable
  code. The two new tests pin both halves: a genuinely empty registry still
  completes a turn (`register_skills=false`), and the default keeps exactly 3
  tools with no plugin contributions.

**2. The capability path was unexercised in production.** Five of eight kinds
had zero production contributors (Command, PromptBlock, StatusSegment, Panel,
Setting), and core *bypassed its own mechanism*: `register_core_status_segments`
and `register_console_panel` wrote the registries directly. Dogfooded:

- `lib/core_segments.cpp` now returns `core_status_capabilities()` instead of
  writing a registry; the wallet readout and the console panel are declared as
  capabilities in `PluginRuntime::install_core_ui()`, installed under a reserved
  `core` owner that `/set plugin` cannot touch (it is not a registered plugin).
- Result, verified by grep: `StatusRegistry::add` and `PanelRegistry::add` are
  each called from **exactly one place**: the capability's own `install`. Core
  and plugin contributions now cannot drift, and an install that is broken
  fails loudly at startup instead of hiding behind "no plugin uses it yet".
- Not ledgered on purpose: the ledger is per plugin and exists to unwind, while
  core UI lives exactly as long as the runtime.
- New test `runtime_core_ui_installs_through_the_capability_path` asserts the
  entries carry owner `core`, which only `PluginServices` can set, so it is
  evidence of the path, not of the outcome.

**3. Two capability kinds were dead in both directions**: no producer and no
consumer anywhere in production, only their own wrapper plus one test each:
`CommandCapability` + `CommandRegistry`, and `SettingCapability` +
`PluginSettingsStore`. Deleted, along with their `CapabilityKind` values, the
console's kind names, the runtime members/accessors, the placeholder registries
in `register_default_tools`, and their tests. `PluginServices` shrank from 8
constructor arguments to 6.

- **Deviation from the plan, stated plainly:** the proposal was to prune
  PromptBlock too. On inspection it is a different case, `PromptRegistry` has a
  live **consumer** (the agent renders its blocks; both hosts wire it in), so it
  is an *unused extension point*, not dead code, unlike Command/Setting which
  had nothing on either side. Kept, and marked "(no caller yet)" in the
  availability table. Removing it is a small follow-up if we want strict D22.

- **Verification:** `./run_tests` → **694 passed, 0 failed**; `make check`
  clean (AGENTS.md audit refreshed for `tui/tui_input.cpp`); `make all` with no
  warnings, including the `panel_view.cpp` one below.

**4. A compiler warning, fixed at the cause.** `tui/panel_view.cpp` produced five
`-Wdangling-pointer` warnings from unnamed temporaries in the footer
constructor, rebuilt on every keypress inside the panel loop. Hoisting the
ternary was not enough (the temporary moved); building the vector with
`push_back` removed it and silenced the warning with **no suppression**. The
footer is now built once instead of per keystroke.

### 2026-09-10, Open findings (need a decision, not more code)

One item is deliberately not implemented, recorded here with the reason and the
trigger that reopens it, so it cannot be mistaken for an oversight.

**(1) ~~Core prompt blocks on the registry~~, RESOLVED 2026-09-11.** The
behaviour question below was answered by reading the history rather than
guessing: the pre-compression memory block was a **regression** from the
immutable-Context rewrite, not a design choice. Fixed by assembling every block
at one point after the gate (`Agent::inject_prompt_blocks`), which also settles
the layout question, the head/tail placement is preserved, so ordinary turns
are byte-identical. The "migrate the core blocks onto the registry" part was
**deliberately not done**, because the registry is shared across windows and
those blocks are per-agent state; the reasoning is in
`docs/spec/agent-loop/prompt-assembly.md`. The original entry is kept below for
the record.

<details>
<summary>original entry</summary>

Two items are deliberately not implemented. Both are recorded here with the
reason and the trigger that reopens them, so they cannot be mistaken for
oversights.

**(1) Core prompt blocks on the registry, blocked on a behaviour decision.**
The framework half is done: plugin blocks render (`PromptRegistry`), each as
its own system message at the tail of the prompt copy, with the sealed
`Context` untouched. Migrating amber's *own* four injections
(`lib/agent.cpp`:190-204 memories, :315-337 skill discovery, :338-346
activated bodies, :349-363 brief) is what remains, and reading them closely
surfaces a behavioural question that is not ours to decide unilaterally:

- The memory block is injected **before** the compression gate
  (`agent.cpp:267-284` vs `:287-313`), and a successful compression
  **reassigns** `prompt_copy` from the rebuilt context (`:309-310`). So on a
  compressing turn the retrieved memories are silently discarded, while the
  skill-discovery block, injected after the gate, survives.
- That asymmetry looks unintended, but "fixing" it changes what the model
  sees on every compressing turn, and moving the blocks to a single tail
  render also changes the layout of *every* request (memory would sit after
  the conversation instead of directly after the system prompt).
- Both are measurable, not arguable: the bench harness exists for exactly
  this. The migration should land with a before/after run, and with the
  layout choice made explicitly rather than as a side effect of a refactor.

**Trigger:** a decision on (a) whether the pre-compression memory block is a
bug, and (b) whether the tail layout is acceptable. Then it is a small change
plus a bench comparison.

</details>

**(2) Host services (`ask`/`choose`/`confirm`/`notify`/`post_to_ui`), no
consumer yet.** The one real need identified ("provider token input") is
already served: `/set provider` prompts for a missing key and the 401
recovery path prompts again, both through the existing `AgentHooks` +
promise/queue machinery. Building the full service surface now would be a
plugin-facing API with nothing calling it, the same failure mode as the log
sinks we cut, and the reason the old `void*` capability struct existed.

**Trigger:** a plugin that needs to ask the user something the core flows do
not already cover (a provider plugin's own setup wizard, an interactive
model picker). The implementation shape is already specified in the spec §8
and reuses the existing modal + promise pattern, so nothing is lost by
waiting for the caller.

**(3) Wallet and Allowance are two mechanisms for one concept, RESOLVED
2026-09-12.** `wallet` survived as the single word: it is the surface users
already type and read, it is already the config key, and "allowance" names only
one of the shapes (quota windows) rather than the question both answer. One
registry, one flag, one segment, one command pair, with the richer snapshot as
the payload. The original entry is kept below for the record.

<details>
<summary>original entry</summary>

**Wallet and Allowance are two mechanisms for one concept.** Found while consolidating, not from a failure: nothing is broken,
but the same idea now exists twice, which is the inconsistency this project
keeps having to unpick later.

- **The evidence.** `WalletRegistry` (`include/agent/extensions.h:193`) and
  `AllowanceRegistry` (`:238`) have the same contract, `Fetch`,
  `add(owner, fetch)`, `find(owner)`, `items()`, `size()`, and the allowance
  header says so in as many words: *"Same shape as WalletRegistry"*. Both are
  runtime-owned (polling, caching, rendering). The duplication reaches the
  surfaces: two config flags (`wallet_enabled`, `allowance_enabled`), two
  status segments (`wallet` priority 800 / drop 9, `allowance` 790 / drop 8,
  **both can render side by side**), and two command pairs
  (`/get|/set provider wallet` and `/get|/set provider allowance`).
- **They are not even different concepts.** `AllowanceSnapshot`
  (`plan`, `windows[]`, `credits_balance`, `unit`, `currency`) is a strict
  **superset** of the wallet's `optional<double>`, the wallet's value is
  `credits_balance` with the extra fields unset.
- **The cost.** Four providers split across the two: kilocode and openrouter
  declare wallets, commandcode and opencode_go declare allowances. A user asks
  one question ("what is left?") and learns two surfaces; the next provider
  author must pick one and can only guess; and the two paths will drift, since
  they are already separate code.
- **Recommendation.** One mechanism, one registry, one flag, one segment, one
  command pair, with the richer snapshot as the type (nothing is lost, the
  wallet's number is the snapshot's `credits_balance`). Keep the bar readout
  wordless (`$13.22`) as specified; let the command surface carry
  plan/windows/credits. Which word survives is a single coin-flip worth making
  once rather than twice; the mechanism should not be named differently from
  the surface it backs.
- **Trigger:** it is a decision, not a discovery, the change itself is
  mechanical (delete one registry, retarget two plugins, fold one flag and one
  segment). It should happen before more providers are written on top of it.

</details>

### 2026-09-10, PF-3.2: panels and the registry console

- **Landed**
  - **`PanelRegistry` + `PanelCapability`**: a panel is a title, a pure
    `lines(width)` renderer, and an optional `on_key` that gets first refusal
    while focused. The host owns placement, framing, scrolling, cycling and
    closing, so a plugin never learns what a window is (D5).
  - **The registry console** (`lib/plugin_console.cpp`), core UI built on the
    *same* public API a plugin uses, registered by the runtime before any
    plugin can contribute a panel. It lists every plugin with tier, state,
    version and contributions, plus the registered panels (D12: always
    available, never hideable by a plugin).
  - **One formatter for both surfaces:** `/get plugin list` and the console
    print the same lines from `plugin_console_lines()`, so what the scrollback
    says and what the panel shows cannot drift.
  - **TUI:** `/panel` (new command-tree node) and **Alt+0** open the view; Tab
    cycles panels, Up/Down/PgUp/PgDn/Home/End scroll, Esc/q closes. Alt+0 was
    free, windows occupy Alt+1..9.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **620 passed, 0 failed** (6 new: ordering, width, key routing, removal,
  capability install/unwind, console content before/after a toggle), `make
  check` → all invariants hold, cppcheck clean (it caught a redundant loop
  guard, fixed). Live: Alt+0/`/panel` renders
  `plugins: 6 registered, 6 on` with `provider:anthropic`, `provider:openai`,
  `provider:gemini`, `provider:openai, segment:kilo_balance`, and
  `panels: 1  plugins  core  Plugin registry`.
- **Not covered:** host services (PF-3.3), a plugin still cannot ask the user
  for input except through the existing API-key path; core prompt blocks
  (PF-1 follow-up).

### 2026-09-10, PF-4 complete: zero hardcoded providers

- **Landed**
  - **`custom` is a plugin** (`plugins/custom/`). The core no longer declares
    any provider: `lib/providers_repo_static.cpp` and
    `make_static_provider_repository()` are **deleted**, and
    `make_default_provider_service` composes only plugin presets + the user's
    files. With no plugins registered the core offers **0** providers, proven
    by test and by probe.
  - **Vendor dialect registration left the core.** `lib/dialect.cpp` registers
    only `openai` (the transport's own protocol); anthropic's dialect is
    registered solely by the anthropic plugin, as gemini's is by the gemini
    plugin. A disabled plugin takes its protocol with it.
  - **`seed_custom_provider` → `seed_provider(name, connection)`**: the core
    knows the `~/.config/amber/providers/<name>.conf` convention, not which
    providers exist. An empty name is refused rather than writing `*.conf`.
  - **The TUI's custom first-run flow is generic**: any provider reporting "no
    endpoint configured" offers the form, instead of the code naming one
    provider. A stale special-case in the provider list (three hardcoded ids
    choosing between two identical branches) went with it.
  - **Build:** five near-identical per-plugin rules collapsed into one pattern
    rule; a new plugin directory now needs only its object entry.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **614 passed, 0 failed**, `make check` → all invariants hold, cppcheck clean.
  Probe against the built library: a fresh install lists custom + the four
  vendors; `plugins/custom/plugin.conf` `enabled=0` removes exactly `custom`
  while the rest keep working; `enabled=1` restores it; and a user's own
  `custom.conf` still supplies the endpoint with the plugin off (the file layer
  is user data, deliberately independent of the preset).
- **Deliberate exception (recorded, not hidden):** `Config::provider_name`
  still defaults to `"custom"`. That is a *default selection*, not a provider
  definition, the core declares no provider, it names which one a fresh config
  points at, and the two host paths that persist an API key need a name for the
  file. When provider selection itself becomes a plugin concern in the
  microkernel work, this default goes with it.

### 2026-09-10, PF-3.1 + PF-4: the status bar registry and the provider conversion

- **Landed**
  - **PF-3.1 status bar**: the bar is composed from `StatusRegistry`. Segments
    return text plus a semantic `StatusTone` (the host maps it to the palette,
    so a plugin never names a colour pair) and are pure functions of a
    `StatusSnapshot` the host publishes each frame. Amber's own segments are
    registered the same way, priorities preserve the previous order exactly,
    and drop priorities preserve what was cut first on a narrow terminal.
    `agent::bar` gained `utf8`/`emdash`/`up`/`down`/`reasoning_badge` so a core
    segment needs no UI header; the TUI's glyph helpers forward to them.
  - **PF-4 provider conversion**: openrouter, kilocode, anthropic and gemini
    are plugins. `capability_overrides()`, `ProviderCapabilities` and
    `Config::api_key_is_account_token` are **deleted**; `Provider::flavor` plus
    the plugin's capabilities carry everything. The static preset repository
    keeps only `custom`, the one provider that is not a vendor.
  - **The kilo balance readout moved into its provider**: poll loop, atomic
    cache and status segment now live in the kilocode plugin, which owns the
    endpoint and the account-token convention. The TUI has no idea kilo.ai
    exists, and `StatusSnapshot::balance_label` (the transitional field) is
    gone.
  - **Supporting design (recorded as decisions):** `IPlugin::tick()` (the host
    tick is forwarded; rendering stays pure), live config via
    `attach_config` (a plugin reading an API key must see what the user typed,
    not a startup copy), declarations split from activation (a plugin that
    ships off still declares its protocol), and presets-only provider
    capabilities (a provider speaking a shared protocol must not take that
    protocol down with it). `PluginRegistry::context()` deleted, dead API
    whose static fallback masked a wiring bug (FIX-018).
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests` →
  **612 passed, 0 failed**, `make check` → all invariants hold, cppcheck clean.
  Live probe of the built binary: `/provider list` reports
  custom/openrouter/kilocode/anthropic/gemini with their true flavors, and
  writing `enabled=0` into `plugins/kilocode/plugin.conf` removes kilocode from
  the list while the rest keep working.
- **Bug caught by the suite during this work:** the presets-only declaration
  marked the *shared* openai flavor unavailable, which would have refused every
  OpenAI-compatible request. Caught by the existing wire tests, fixed by making
  the unavailable mark apply only to flavors nobody is currently providing.
- **Not covered:** host services (PF-3.3) and panels/console (PF-3.2); core
  prompt blocks (PF-1 follow-up).

### 2026-09-10, PF-2.1–PF-2.6: provider plugins, Gemini, race-free registry

- **Landed**
  - **PF-2.1 Provider capability**: `ProviderCapability` carries flavor +
    dialect factory + presets; `register_provider_preset`/
    `unregister_provider_presets_for` put plugin presets in the ordinary
    repository merge, so a plugin provider is an ordinary provider row.
  - **PF-2.2 Registry resolution**: the dialect table is mutex-guarded;
    unregistering marks a flavor *unavailable* rather than forgetting it, and
    `HttpLLMClient` refuses to construct for a disabled plugin's flavor with a
    message naming the plugin and the fix. Unknown flavors still fall back.
  - **PF-2.3 Provider-driven catalog**: `Provider::flavor` now reaches the
    model catalog, which fixes `/provider test`: it previously probed every
    provider as OpenAI. `apply_selection` takes the flavor from the provider
    itself, so the name-keyed lookup is down to the kilocode account-token
    quirk alone (PF-4 deletes it).
  - **PF-2.4/2.5 Gemini dialect + plugin**: `generateContent` /
    `streamGenerateContent?alt=sse`, `x-goog-api-key`, `contents`+`parts`,
    `systemInstruction`, `functionDeclarations` with the schema inline,
    `functionCall`/`functionResponse` with synthetic call ids,
    `usageMetadata`, model listing that strips the `models/` prefix, its own
    overflow prose. `plugins/gemini/` contributes it; the bundled list carries
    it.
  - **PF-2.6 `flavor` in provider files**: parsed and written; the default is
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
  a *live* Gemini response (hermetic fixtures only, as specified, no live
  provider calls in the suite).

### 2026-09-10, PF-1.2, PF-1.4–PF-1.7: the framework runs

- **Branch:** `feat/plugin-framework-pf1` (pushed; no PR yet, the feature is
  not finished to a shippable state).
- **Landed**
  - **PF-1.2 publish sites**: `Agent::set_events`; TurnStarted (before the
    prompt is sealed, so an interceptor can rewrite it), TurnEnded,
    MessageAdded at every push site, LlmResponse, CompressionTriggered/
    Completed, ErrorRaised; tool events in dispatch with an interceptable
    ToolRequested that can rewrite arguments or veto a call. Hidden
    confirmation exchanges publish nothing.
  - **PF-1.4 registries**: prompt blocks, commands, per-plugin settings,
    owner-tagged, plus four concrete capabilities (tool, command, prompt
    block, setting). `ToolRegistry::remove_tool`.
    The dead `void*`/Type-enum `Capability` struct is deleted; `IPlugin::
    capabilities()` returns owned typed capabilities, called once at
    activation.
  - **PF-1.5 runtime**: composition root, ledger install/unwind,
    `~/.config/amber/plugins/<id>/plugin.conf`, bundled-on-by-default,
    `set_state` as the single write path. TUI: `/get plugin [list|<id>]`,
    `/set plugin <id> on|off` as feed leaves, agents get the bus and the
    prompt registry. CLI: same runtime, `--no-plugins`.
  - **PF-1.6 v1 adapter**: `ExternalPluginAdapter` puts discovered external plugins
    under the same surface (off by default; first-wins on id collision).
  - **PF-1.7 dogfood (partial)**: metrics is live; an end-to-end test drives a
    real Agent turn with the bus attached and asserts the plugin counted it,
    then that disabling leaves the harness empty.
- **Verification:** `make clean && make && make test` (exit 0), `./run_tests`
  → **583 passed, 0 failed**, `make check` → all invariants hold, cppcheck
  clean on the new sources. Live smoke: TUI drawer shows `/get plugin` →
  `metrics on, bundled v1.0.0`; headless CLI run completes with the runtime
  active.
- **Deviation (recorded, not hidden):** PF-1.7's second half, migrating the
  **core** prompt blocks (memory, skill discovery, session brief) onto the
  PromptRegistry, is **not done**. Plugin blocks are appended after the core
  blocks today, which is behaviour-preserving and keeps the KV prefix stable.
  The core migration changes prompt assembly for every request, so it wants its
  own focused pass rather than the tail end of a long one.
- **Not covered:** PF-1.6's adapters are built at startup; a plugin installed
  mid-session appears after a restart (the v1 install path still re-runs
  discovery, but adapters are not rebuilt). Nothing consumes `CapabilityKind::
  Provider` yet, that is PF-2.
- **Environment caveat:** `clang-format` is not installed on this machine;
  `make format-check` could not run locally. New files follow the
  LLVM/4-space/100-col style and were checked by hand. CI must confirm.

### 2026-09-10, PF-1.1 + PF-1.3: typed events and the ledger

- **Branch:** `feat/plugin-framework-pf1`.
- **Landed**
  - `include/agent/events.h`, typed payload catalogue, `EventTraits` mapping,
    RAII `Subscription`, `Events` facade (`subscribe` / `intercept` / `publish`).
  - `include/agent/event_bus.h`, `lib/event_bus.cpp`, per-type subscriber
    counts + `has_subscribers()`; the typed `publish()` returns on a single
    relaxed atomic load when nobody listens (invariant 5). Enum gains
    `CompressionCompleted`, `ErrorRaised`, `Count`.
  - `include/agent/plugin_capability.h`, `lib/plugin_capability.cpp`, typed
    `Capability`, `Contribution`, `InstallResult`, the `PluginServices` anchor,
    and `PluginLedger` with reverse-order unwinding that cannot be aborted by a
    throwing `remove` handler.
  - `tests/plugin_framework_test.cpp`, 21 tests: typed delivery, type
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
- **Not covered (next):** PF-1.2 publish sites, nothing publishes yet, so typed
  subscription is not observable in a real turn; the capability protocol is not
  yet consumed by a runtime; `PluginServices` sinks and the registries are
  PF-1.4; `/get plugin` / `/set plugin` is PF-1.5.
- **Gotchas worth keeping**
  - Editing `Makefile.in` does **not** regenerate `Makefile`: `GNUmakefile` only
    regenerates when the Makefile is *missing*. Run `./configure` after touching
    build files, or the new source is silently not compiled.
  - `ar rcs` never drops stale members, so a deleted source's `.o` stays in the
    archive until `make clean`, which is how a retired `request_builder.o`
    resurfaced as a duplicate-symbol link error.
  - Pre-existing `-Wunused-variable` warning in `plugins/metrics/metrics_plugin.cpp`
    (unused `end_sub`); PF-1.7 migrates that plugin onto the typed API and
    removes it.

---

## Decision Log

Binding. Superseding a decision requires editing the spec in the same change.

| # | Decision | Rejected alternative (and why not) |
|---|---|---|
| **D1** | Three mechanisms: **contribution registries** (existence), **events** (occurrence), **host services** (requests). Registration never happens via events. | One generic "capability bag" routed by the registry, routing rules for timing/ordering/deactivation become unspecifiable. |
| **D2** | Capabilities are **typed polymorphic objects**; the runtime installs them and records each install as a ledger entry. | (a) `void* impl` + enum (untyped, UB on a wrong cast, unpublishable API); (b) plugin-called imperative registration (deactivation correctness becomes the author's problem; leaks callbacks into freed state). |
| **D3** | **Typed event layer over the existing tested `EventBus`**; payload structs in `events.h`. | (a) Replacing the bus (churns pinned, tested semantics for no gain); (b) keeping `void*` payloads (out-of-band cast contract); (c) synthesising events from `AgentHooks` (compression, errors, and request/response are not hooks, the bus would lie). |
| **D4** | **Pull for UI state, push for lifecycle.** Segments/panels are callables the host invokes on the UI thread; events push to plugins; `post_to_ui` is the sanctioned cross-thread update. | Pushing UI state from worker threads, forces locking inside render and makes frame composition non-deterministic. |
| **D5** | **Declared UI regions** (status segments, panels) with the host owning layout, focus, and overflow; modals only through host services. | Arbitrary geometry/z-order/window hijack, requires a layout-engine rewrite and couples plugins to TUI internals; raw rendering makes every plugin a UI fork. |
| **D6** | **Host services** (`ask_text`, `ask_secret`, `choose`, `confirm`, `notify`, `post_to_ui`, `log`) implemented per host; TUI reuses the promise + queue + deferral machinery that already backs approvals. | Plugins calling TUI widgets directly, no CLI parity, couples the plugin to ncurses, re-entrancy hazards with the modal system. |
| **D7** | **One composition root (`Runtime`)** constructed by both hosts. | Per-host wiring, the CLI-has-no-plugins asymmetry is the bug, not the baseline. |
| **D8** | **Provider capability = flavor + dialect factory + presets + auth spec.** Behaviour lives in the dialect; the plugin supplies data. No boolean capability flags. | (a) `ProviderImpl` with chat/stream function objects (bypasses the dialect seam, duplicates transport, two mechanisms for one job); (b) `supports_x` booleans (the boolean explosion the dialect work deliberately deleted). |
| **D9** | **State layout** `~/.config/amber/plugins/<id>/plugin.conf` + `$(datadir)/amber/plugins/<id>/` for bundled assets, same shape the future external tier will use. | Putting plugin state in the global config or next to the binary, breaks the FHS/prefix resolution rules the repo already established. |
| **D10** | **`kPluginApiVersion` gate, deferred to PF-6.** For compiled-in plugins the compiler *is* the version check; a runtime constant that can only ever match is ceremony. It becomes load-bearing when code can arrive that was not built with the harness (the external tier). | Silent acceptance in PF-6, an incompatible out-of-tree plugin must fail loudly there. For PF-1..PF-5 the alternative is dead weight, which the engineering principles ban. |
| **D11** | **Performance invariants** (spec §6): unsubscribed publish is a single atomic load; no per-token events; no allocation on the fast path; no duplicate of hook traffic. | "Optimise later", a bus that costs per token is unusable in the streaming path and would have to be redesigned. |
| **D12** | **Console is core UI built on the public panel API** (Alt+0), always available; plugins may add panels too. | Making the console itself a plugin, a disabled plugin must never be able to hide the registry view used to re-enable it. |
| **D13** | **Compiled-in core plugins first**; runtime loading is not in the shipped design, see **D16** for the tier decision and its reasoning. | Shipping a loader now, needs an ABI contract, trust model, and install story that do not exist yet. |
| **D14** | **Error classification stays in the dialect**; the bus only *observes* errors. | An interceptable error event, classification is provider-specific (already `is_retryable` / `context_overflow_hint`); a second place to decide it is a bug factory. |
| **D15** | **Bundled plugins are enabled by default**; a user disables one for performance or preference, and a disabled plugin costs nothing at runtime (no subscriptions, no registry rows, no constructed dialect, invariant 8). | Opt-in activation, the default state would ship a harness with its own extensions switched off, and "enable to get the shipped behaviour" is a worse first run. |
| **D16** | **Tier by isolation, not by size.** Bundled (compiled-in) is the only shipped tier; the process tier is deferred to PF-6 and gets a *different* capability shape (an external provider owns its transport and credentials); `dlopen` is **rejected**; threads are not a tier. | (a) `dlopen`, buys install-without-rebuild at the cost of a fragile C++ ABI contract and keeps the shared crash domain; the process tier is strictly better for that goal. (b) Process-everything, the streaming decoder runs per SSE chunk, so bundled providers must never cross an address space, and every process provider would re-implement retry/timeout/cancel. (c) Threads as isolation, same address space, same crash domain; threads are parallelism only. |
| **D17** | **One write path for plugin state: `/set plugin <id> on\|off`** (plus `/set plugin <id> k=v`), read via `/get plugin list\|<id>`; `/plugin` keeps packaging verbs (`install`/`uninstall`/`info`) and drops `enable`/`disable`. Toggling re-publishes the feeds the plugin fed (provider list, command tree) with no restart. | Keeping `/plugin enable` *and* `/set plugin on`, two write paths to one state, which drift; it is the "no dead legacy dispatch" rule applied to the new surface. |
| **D18** | **Rendering stays pure; time-driven work happens in `IPlugin::tick()`** (forwarded from the host's UI tick; must not block). | A segment fetching on render, it runs inside frame composition, so I/O or thread spawns there would make every paint unpredictable. The tick is what lets a plugin refresh a remote value while its segment stays a pure read of the cached result. |
| **D19** | **Declarations are separated from activation:** the runtime reads a plugin's declared capabilities at *registration* and installs them at activation. | Installing-only knowledge, a plugin that ships switched off would leave no trace, so a provider file pointing at its flavor would silently fall back to another wire protocol instead of reporting the disabled plugin. This is also what the console will use to answer "what would this plugin add?". |
| **D20** | **A provider capability may contribute presets with no dialect factory** (presets-only), and a presets-only provider never takes a shared protocol down with it. | Requiring a factory, an OpenAI-compatible gateway (kilocode, openrouter) would have to claim ownership of the shared `openai` dialect, so switching that one provider off would break OpenAI-compatible use everywhere. |
| **D21** | **Plugins read the host's LIVE config** (`PluginRuntime::attach_config`), never a startup copy. | A snapshot taken at construction, a plugin resolving an API key or the active provider would act on stale state after the user changes either. |
| **D22** | **No plugin-facing surface without a caller.** Host services and log sinks are specified but unbuilt until something calls them. | Building them now, a plugin API nothing uses is exactly the state this rebuild started from (`capabilities()` read only by tests, a bus nobody fired), and it makes the surface's real shape unknowable while it is still unproven. The spec keeps the design; the tracker keeps the trigger. |

---

## Availability Table

What a plugin author can rely on today. Update with every landed task.

| Capability | Status | Phase |
|---|---|---|
| External tool plugin (subprocess, JSON-RPC) | ✅ Shipping | v1 (unchanged) |
| Tool contribution (core plugin) | ✅ | PF-1 |
| Core tool set as a plugin (`plugins/core_tools`, 9 tools) | ✅ | PF-4.4 |
| Harness services injected into capability factories | ✅ | PF-4.4 |
| Declining capability leaves its plugin active | ✅ | PF-4.4 |
| Command contribution (executable) | – | Removed 2026-09-11 (no producer, no consumer) |
| Event subscription (typed) | ✅ | PF-1 |
| Enable/disable with clean unwinding | ✅ | PF-1 |
| v1 external plugins under the unified registry | ✅ | PF-1 |
| Prompt block contribution | ✅ | PF-1 (no caller yet) |
| Plugin settings store + contribution | – | Removed 2026-09-11 (no producer, no consumer) |
| `/get plugin`, `/set plugin on\|off` (persisted, live) | ✅ | PF-1 |
| Core UI on the capability path (dogfood) | ✅ | 2026-09-11 |
| Provider contribution (dialect + presets) | ✅ | PF-2 |
| Provider presets only (shared protocol) | ✅ | PF-4 |
| Provider list reflects plugin state without restart | ✅ | PF-4 |
| Config-only OpenAI-compatible provider | ✅ | shipping |
| `flavor` in provider files | ✅ | PF-2 |
| Status segment contribution | ✅ | PF-3.1 |
| Time-driven work (`IPlugin::tick`) | ✅ | PF-3.1 |
| Every vendor provider shipped as a plugin | ✅ | PF-4 |
| Panel contribution + registry console | ✅ | PF-3.2 |
| Plugin description + category (grouped registry list) | ✅ | PF-1 |
| Wallet contribution — balance and/or quota windows (fetch only; polling + rendering core) | ✅ | PF-3.4, unified 2026-09-12 |
| `/get provider wallet`, `/set provider wallet on\|off` | ✅ | PF-3.4 |
| OpenCode Go provider (presets + wallet) | ✅ | PF-3.5 |
| OpenCode Zen provider (presets only) | ✅ | PF-3.5 |
| CommandCode provider (presets + wallet) | ✅ | PF-3.5 |
| DeepSeek provider (presets + wallet) | ✅ | PF-3.5 |
| Host services (ask/choose/confirm/notify) | ⏳ | PF-3.3 |
| Log sinks | – | Deferred (no consumer) |
| Theme, key interception, geometry, hot reload | – | Deferred Register |
| External (process) tier | – | PF-6 (deferred, shaped for) |
| `dlopen` loadable library | ❌ rejected | D16 |

---

## Phases

### PF-1, Framework core (typed events, capabilities, ledger, registries)

**Proves:** a compiled-in plugin can be enabled, contribute, and be disabled
cleanly on both hosts, with the dormant metrics plugin as the live consumer.

| Task | Scope | Spec refs |
|---|---|---|
| **PF-1.1** Typed events | `include/agent/events.h` payload structs + typed `publish`/`subscribe` over `EventBus`; unsubscribed fast path | §6, PLG-03, PLG-04 |
| **PF-1.2** Publish sites | Fire the catalogue at the verified sites (turn, message, tool, LLM response, compression, error) behind an optional `EventBus*` port on `Agent`; never publish hidden confirmation exchanges | §6, PLG-03 |
| **PF-1.3** Capability protocol + ledger | Typed `Capability`; `InstallResult`/`Contribution`; `PluginLedger` with reverse unwinding | §3, PLG-01 |
| **PF-1.4** Registries | Tools (owner-tagged add/remove, const bug fixed), Commands (subtree + handlers), Prompt blocks, Settings; common `ExtensionPoint` introspection view. **No log sinks**: cut, no consumer | §4, §5, PLG-02, PLG-05 |
| **PF-1.5** Runtime + host wiring | Composition root; plugin state persistence (`plugin.conf`, bundled plugins **on by default**); `/get plugin list\|<id>` + `/set plugin <id> on\|off` as the single write path (D17); default-on applied at first boot; CLI parity | §4, §9, PLG-10, PLG-11 |
| **PF-1.6** v1 tier under the same registry | `ExternalPluginAdapter` wraps the external `PluginManager` in `IPlugin`, so external plugins appear in `/get plugin list` and are toggled by `/set plugin <id> on\|off`. **This is what makes deleting `/plugin enable\|disable` safe**: without it, D17 strands the external tier with no control surface | §2, §9, PLG-11 |
| **PF-1.7** Dogfood | Metrics plugin activated on both hosts; core prompt blocks (memory/skills/brief) migrated onto the prompt registry; `chat_once` injection logic deleted | §5, §6 |

**Gate:** ledger test proves disable restores the registries exactly; prompt
blocks are order-stable across turns; metrics shows live numbers in CLI and TUI;
`make check` green.

### PF-2, Provider capability + Gemini

**Proves:** a genuinely incompatible vendor lands as a plugin with zero shared
core edits, the whole point of the exercise. Sequenced **ahead of the UI
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

### PF-3, UI contribution surfaces

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

### PF-4, Convert the built-ins

**Proves:** the framework can absorb amber's own provider handling, deleting the
last name-keyed branching instead of adding to it.

| Task | Scope | Spec refs |
|---|---|---|
| **PF-4.1** kilocode plugin | Balance fetch/readout moves out of `lib/model_probe.cpp` into the plugin as a status segment + auth semantics | §4, §7 |
| **PF-4.2** openrouter + anthropic plugins | Convert to `ProviderSpec`; delete `capability_overrides()`; `Config` loses the kilo/account-token fields | §7 |
| **PF-4.3** Docs alignment | Provider specs re-aligned; `flavor` documented as provider data | §7 |
| **PF-4.4** Core tools plugin | `register_default_tools`' factories become `ToolCapability` factories in `plugins/core_tools`; `HostServices` (jobs/todos/subagents/cancel token) is injected at install, since a tool binds to host state | §3, §8 |

**Gate:** `grep provider_name ==` → 0 in `lib/`/`include/`; no feature regressions
in the TUI provider flows; dialect tests untouched.

### PF-5, Contributor enablement

**Proves:** someone outside this repo can build a plugin without reading the
framework source.

| Task | Scope |
|---|---|
| **PF-5.1** Developer guide finalized | Availability table accurate, examples compile, threading rules explicit |
| **PF-5.2** Scaffold | `plugins/_template/` with a minimal, tested plugin and build wiring |
| **PF-5.3** API compatibility policy | What `kPluginApiVersion` covers, how breaking changes are staged |
| **PF-5.4** External-tier review | Decide whether/when non-tool capabilities cross the process boundary, the entry condition for PF-6 |

---

### PF-6, External (process) tier, DEFERRED

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
  (`lib/plugin.cpp` v1 protocol; `lib/mcp_transport.cpp`), not a third protocol.
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
| log sinks | Cut from PF-1 during sign-off: listed in the registry set with no phase naming a consumer, the exact "half-surface" failure this design exists to avoid | A component needs structured log fan-out (the conversation log stays authoritative) |
| `kPluginApiVersion` | Deferred with the external tier (D10): for compiled-in plugins the compiler is the version check | PF-6 |
| external (process) tier | Needs a wire contract, trust model, and install story; the in-process interface must be proven first so its wire form is knowable | PF-6 entry condition above |
| `dlopen` loadable library | Rejected (D16), not deferred: fragile C++ ABI contract, same crash domain, no language freedom, the process tier dominates it | Only if PF-6 is rejected *and* a concrete need for no-rebuild install remains |
| Bedrock/Vertex request signing | Already noted as a dialect-seam follow-up (`docs/fix-tracker.md`) | A provider plugin needs signature auth, the `AuthSpec` seam accommodates it |

---

## Risks

| Risk | Mitigation |
|---|---|
| The framework grows eight half-surfaces no one uses (the current state, rebuilt) | Every phase names its consumer; a registry without a driving consumer is not done; the console makes dead surfaces visible |
| `Tui` is a friendship/ownership web; plugin hooks could pierce internals and block a future public API | Plugins depend only on registries and host services; TUI-specific code stays behind those interfaces; the console is the proof |
| Event overhead in the streaming path | Hard invariants (D11) + a no-subscriber test; per-token publishing is prohibited |
| Deactivation leaves dangling callbacks | Ledger with reverse unwinding + a test that diffs registry state before/after disable |
| Provider conversion (PF-4) regresses the working provider flows | Convert behind the existing tests; the dialect seam already isolates wire behaviour; convert one provider per PR |
| Contributor documentation describes unbuilt surfaces | The guide's availability table is the single status surface and is updated with each landing task |

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
   the single status surface.
6. `make clean && make && make test && make lint && make analyze && make check`
   green on g++ and clang++, zero new findings.
