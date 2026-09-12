# Spec: The Tools Domain as Plugins

Status: **proposal for review** (nothing here is implemented yet)
Supersedes: nothing. Extends `plugin-framework-v2.md` §4 (extension points).
Tracker: `docs/plugin-framework-tracker.md` (PF-5).

## Purpose

The plugin framework exists; the tool domain does not use it beyond the
minimum. This document collects every idea, decision and open question for
turning the tool set into first-class plugins: per-tool metadata, correct
display, per-tool prompts, and enough granularity to run controlled
experiments ("switch off `search`, see how the agent copes").

It is written to be argued with. §4 lists what is decided, §5 what is not.

## 1. Where we actually are

Established by reading the tree at `main` (`2eb3214`), not from memory:

| Thing | State | Evidence |
|---|---|---|
| The core tool set is already a plugin | ✅ 9 tools from 7 capabilities in `plugins/core_tools/` | `plugins/core_tools/core_tools_plugin.cpp:27` |
| One definition, two install paths | ✅ `register_default_tools` installs the *same* capabilities directly for hosts with no runtime | `lib/tools_default.cpp:17-40` |
| `IPlugin` meta | ✅ `id`, `version`, `name`, `description`, `category` | `include/agent/plugin_v2.h:47-58` |
| Plugin categories + console grouping | ✅ core vocabulary, open set, unknown categories get their own heading | `plugin_v2.h:22-30`, `lib/plugin_console.cpp:44-70` |
| Tool self-description | ✅ `name`, `description`, `parameters_schema`, `requires_approval(args)`, `is_read_only`, `summarize(args)` | `include/agent/tool.h:26-62` |
| Tool display | ❌ **hardcoded name→verb table and a name `if/else` chain** | `tui/tool_display.cpp:26-40`, `:72-100` |
| Tool prompts | ❌ one static `prompts/tools.md`, loaded wholesale | `lib/agent.cpp:156-167` |
| Prompt-block contribution | ⚠️ implemented, **no caller** | `include/agent/extensions.h:370`; tracker availability table |
| Skill tools | ❌ registered directly by the agent | `lib/agent.cpp:98` |
| MCP tools | ❌ registered directly — **parked by decision, see §6** | `lib/mcp_tools.cpp:101` |
| Experiment harness | ✅ scenario loader, oracle scorer, KPI aggregation (hermetic + live) | `bench/` |

Two consequences worth stating plainly:

1. **The category mechanism already exists and is exactly what was asked for.**
   "Tools in the correct category driven by metadata" needs no new vocabulary:
   a per-tool plugin declares `category()`, and the registry list already groups
   by it. The only work is *splitting the plugin*, not inventing categories.
2. **The tool already describes itself.** Adding a parallel metadata layer that
   restates name/description/read-only-ness would be a second source of truth —
   the failure mode this framework keeps having to fix. What is genuinely
   missing is narrow: a display *verb* and per-tool *prompt text*.

## 2. What is being asked for

Stated as requirements, in the words that motivated them:

- **R1** — wire the basic tooling *and its prompts* through the plugin framework.
- **R2** — break the tools into separate plugins, so each can carry its own meta.
- **R3** — display tool names correctly (today a plugin tool renders generically).
- **R4** — allow micro-tuning of individual tools. Example given: `search` is
  strict enough that the agent reaches for `rg` through `bash` instead.
- **R5** — a harness where a whole plugin (e.g. `search`) can be switched off and
  the effect on the agent observed.
- **R6** — tools grouped by metadata-driven category.
- **R7** — *(open)* a type/class with either a hard lock or at least a warning
  when two read tools or two write tools are enabled at once.

## 3. Design

### 3.1 Granularity: one plugin per tunable unit

Not "one plugin per tool" as an absolute — one plugin per thing a user would
want to switch independently.

| Plugin | Tools | Why this boundary |
|---|---|---|
| `tool_read` | `read` | Switchable alone: the safety experiment "no file reads" |
| `tool_write` | `write` | Switchable alone; the natural A/B partner of `read` |
| `tool_search` | `search` | **The pilot** — the tool R4 names as problematic |
| `tool_bash` | `bash` | Its own approval semantics; switchable alone |
| `tool_process` | `process_start/read/stop` | Three tools, one shared job service and lifecycle — splitting would be ceremony |
| `tool_plan` | `todowrite` | Already conditional; see below |
| `tool_task` | `task` | Already conditional; see below |
| `tool_skills` | `read_skill`, `list_skills`, `write_skill` | One catalog, three views (and it moves registration out of `lib/agent.cpp`) |

**Bonus that falls out:** `todowrite` and `task` are enabled today by the
`plan_tool`/`task_tool` config flags, which exist only because those tools can
be absent. Once each is a plugin, **plugin state *is* the flag** — two config
booleans and their plumbing delete themselves. That is D17 ("one write path")
applied to a case where we currently have two.

### 3.2 Meta: reuse what exists, add only the gap

A per-tool plugin already gets, from the existing `IPlugin` surface: `id`,
`version`, `name`, `description`, `category`. Nothing new is needed for R2/R3/R6
— the meta the console shows and groups by is already there.

The one genuine gap is the **display verb** ("searching", "planning",
"hacking"), which currently lives in a table in the TUI. Two candidate homes:

- **(a) `Tool::display_verb()`** — the tool names itself; the TUI already holds
  `agent::ToolRegistry& reg_` (`tui/tui.h:175`), so there is zero new plumbing.
  Precedent: the port already has `summarize()`, documented as shown in
  approval prompts.
- **(b) data on the capability, stored with the owner in `ToolRegistry`** —
  keeps presentation vocabulary out of the tool port; matches
  `WalletCapability` (data) and `ProviderCapability` (data + factory); and is
  the shape the external tier will need, since a process plugin's meta arrives
  as a manifest, not as C++ virtuals.

**Recommendation: (b)**, with the distinction that `summarize(args)` (what
*this* invocation is doing — the tool's own knowledge of its arguments) stays on
the tool, while `verb` (harness vocabulary for a status line) is harness data.
`ToolRegistry` already carries non-tool bookkeeping per entry (`owner`, added
for unwinding in #112), so the meta sits beside it and the UI keeps one lookup.

### 3.3 Display: stop switching on the name

`tui/tool_display.cpp` hardcodes both a verb per tool and per-tool argument
formatting, with a raw-JSON fallback. A plugin-contributed tool therefore shows
as `mytool {"path":...}`.

The fix is not to extend the table — it is to **ask the tool**. `describe_tool_call`
becomes: `meta.verb` from the registry, detail from `Tool::summarize(args)`, with
the raw dump kept as the last resort for tools that supply neither. The hardcoded
entry list and the `name == "search"` branch then delete themselves.

### 3.4 Prompts travel with the tool

Today `prompts/tools.md` is one static document advertising every tool, loaded
wholesale (`lib/agent.cpp:156`). If tools can be switched off, a static document
lies to the model: it is told about a tool that is not in the schema.

Each tool plugin contributes a prompt block instead — **this is the caller
`PromptBlockCapability` never had**, which is why it has sat unimplemented
(tracker: "no caller yet"). Consequences:

- Disabling `search` removes its schema *and* its prompt text in one action. The
  model can no longer be told about a tool it cannot call.
- Ordering stays explicit (the registry already orders blocks by priority).
- The monolithic `tools.md` shrinks to whatever is genuinely cross-tool
  (conventions that apply to every tool), or disappears.

This is the part that makes R1 more than a file move. It is also the piece with
a real risk: prompt changes alter behaviour, so it needs a before/after bench run
(§3.6) — the repo already mandates that for prompt edits.

### 3.5 Category and class

**Category (R6): already solved, use it as-is.** Each tool plugin declares one of
the core categories (`tools`, `search`, `memory`, …) or invents its own; the
console groups by it, ranked, with unknown categories visible rather than hidden.

**Class (R7): the two axes already exist.** `is_read_only()` is the safety axis
and already drives read-mode dispatch; `requires_approval(args)` is the gating
axis and is argument-aware (a read-only `cat` versus `rm -rf`). A third
"class" enum would restate both unless something concrete consumes it.

So the recommendation is **not to add a class enum yet**, and instead to add the
one thing that is actually missing: an **audit** after every enable/disable that
answers "is this toolset still usable?" — see §4.3. The trigger to revisit: a
caller that needs a distinction `is_read_only` + `category` cannot express.

### 3.6 Measuring (R5): the bench must pin plugin state

The harness exists (`bench/`: scenario loader, oracle scorer, KPI aggregation,
hermetic and live). What it does not do is record *which plugins were on*. Since
plugin state now changes agent capability, **a bench result that does not record
it is not reproducible and not attributable**.

Two small additions:

1. Record enabled/disabled plugin ids in the result JSON (and print them in the
   scorecard), so two runs can be compared honestly.
2. A knob to run a suite with a named plugin disabled, so `search off` is one
   command rather than a hand-edited config.

Then R4's actual question — "is `search` losing to `rg` because it is too
strict?" — becomes a measurement: same scenarios, `tool_search` on versus off,
compared on the existing KPIs. Tuning the description or the tool itself is then
guided by a number rather than by feel.

## 4. Decided

- **4.1** Tools are split per tunable unit (§3.1), not one giant `core_tools`
  and not one plugin per tool by dogma.
- **4.2** Categories come from the existing `plugin_category` mechanism; no new
  vocabulary.
- **4.3** Display stops switching on tool names; the tool (or its meta) supplies
  the verb and the invocation summary.
- **4.4** Tool prompts become prompt blocks owned by the tool plugin, so schema
  and prompt cannot disagree about what exists.
- **4.5** Bench results record plugin state; a plugin can be disabled per run.
- **4.6** MCP is parked (§6). Its tools stay registered directly for now.
- **4.7** `search` is the pilot for the whole exercise: it is the tool with a
  concrete, user-observed problem, so it proves the mechanism or fails it early.
- **4.8** Scope is the whole domain in one pass: the eight tool plugins, the
  skill tools (moving a direct registration out of `lib/agent.cpp`), and
  deleting `plan_tool`/`task_tool` (§5.3). Not the two-tool pilot.
- **4.9** The display verb lives as **capability data in `ToolRegistry`**
  (§5.1) — decided and implemented.
- **4.10** The guard is **deficiency and ambiguity warnings, never a lock**
  (§5.2).
- **4.11** Wallet and Allowance are **one mechanism**, called **wallet**, with
  the richer snapshot as its type (§5.5).

## 5. Resolved

**5.1 Where does the display verb live?** (§3.2) **RESOLVED: capability data,
beside `owner` in `ToolRegistry`.** A ToolCapability declares verbs keyed by
tool name; `ToolRegistry::meta_for()` answers the UI. Implemented — see the
"a tool declares its own display verb" commit.

**5.2 What does the "two reads" guard actually do?** (the open R7). Three
candidate behaviours:

- *Hard lock* — refuse to enable a second read tool. **Rejected as the default:**
  it forbids the two-search-implementations comparison, which is precisely the
  experiment R5 asks for. It also cannot be right in general — a stricter and a
  looser search tool side by side is a legitimate configuration, and the model
  arbitrates between them (which is what it is already doing with `rg`).
- *Warn on deficiency* — after any change, audit the resulting toolset and warn
  when a **required** capability is now missing (no read tool at all, i.e. the
  "empty toolbox" finding already open in the tracker). **Recommended.**
- *Warn on ambiguity* — warn when two enabled tools share a category *and*
  have descriptions that do not distinguish them, since that is what actually
  confuses a model. Cheap, and it targets the real symptom.

**RESOLVED: deficiency and ambiguity warnings; no hard lock.** A lock would
forbid the two-search-implementations comparison this work exists to enable, and
two similar tools is a description problem the bench can measure, not a
permissions problem. If a hard gate is ever wanted it belongs in CI/bench as
opt-in strictness, not in the user's session.

**5.3 Does anything still need `plan_tool`/`task_tool`?** **RESOLVED: delete
both.** Plugin state replaces the flag, so the two config booleans and their
plumbing go. (Note for the implementation: the CLI and several tests pass those
flags positionally, so `register_default_tools`' signature changes with it — its
remaining purpose is hosts that hold a bare registry and no runtime.)

**5.4 Do skill tools move too?** **RESOLVED: yes**, as one plugin — it also
removes the last direct tool registration from `lib/agent.cpp`.

**5.5 Wallet vs Allowance.** **RESOLVED: one mechanism, one registry, one flag,
one segment, one command pair, named wallet**, with `AllowanceSnapshot`'s richer
type as the wallet's return type so nothing is lost (plan, windows,
credits_balance, unit, currency). Two providers retarget from allowance to
wallet. Recorded in the tracker as open finding (3).

## 6. Parked

- **MCP** — explicitly parked by request. When revisited, the question is
  whether it is a plugin at all or a *feed*: its tools are runtime-discovered,
  which is a different shape from a compiled-in contribution.
- **External (process) tier** — PF-6, deferred with its entry conditions recorded.
- **Host services** (`ask`/`confirm`/`notify`) — still without a caller; a tool
  plugin needing user input would be the first.
- **Repo-wide clang-format sweep** — 247 files, 8,353 pre-existing violations.
  Separate decision, not part of this work.
- **Memory/compression prompt-block ordering** — the core prompt-block migration
  has its own open question (a block injected before the compression gate is
  discarded when compression rebuilds). Tool prompts in §3.4 are independent of
  it and should not be bundled with it.

## 7. Sequence

1. `tool_search` split out as the pilot plugin, with category and prompt block
   (proves §3.1, §3.4 end to end on one tool).
2. Bench records plugin state + the disable knob (§3.6); run `search` on/off.
3. Tune `search` from the result — description, strictness, or leave it be.
4. Split the remaining tools (§3.1 table).
5. Display meta (§3.2/§3.3): delete the hardcoded table; plugin tools render
   correctly.
6. The audit (§5.2) and whatever 5.3/5.4 decide.
7. Delete the now-empty `core_tools` bundle and the `plan_tool`/`task_tool`
   flags if 5.3 says so.

Step 1 is deliberately small: one tool, end to end, before seven more.

## Cross-references

- `plugin-framework-v2.md` — the framework this extends (§4 extension points,
  §5 prompt blocks, §7 provider plugins as the worked example).
- `developer-guide.md` — how a plugin author writes a capability.
- `docs/plugin-framework-tracker.md` — phases, decision log, availability.
- `bench/README.md`, `BENCHMARK.md` — the harness §3.6 plugs into.
