# Spec Directory

Central contracts for all amber features. Every file here defines *what must
hold* for a feature to be correct. If the code violates the spec, the code is
wrong.

## Reading order

| Step | Document | Purpose |
|------|----------|---------|
| 1 | `VISION.md` | **Why**: inspiration, north star, the spirit. Never changes. |
| 2 | `MISSION.md` | **What and when**: priority ladder, feature filter, phasing, gap register, user promise. |
| 3 | `ux-policy.md` | **How the interface works**: 1:1 mapping, autosuggest, `?`-at-depth, readline, widget policy, habit loops. |
| 4 | Individual specs | **Contracts**: invariants, scenarios, error states, cross-references. |

## Roadmaps

| File | Covers |
|------|--------|
| `roadmap/0.6.0.md` | Build-ready 0.6.0 plan: command foundation, work surfaces, project sessions, issue numbers, dependencies, release gates, current workspace state |

## Gap register

See `MISSION.md` → Gap register for the complete list of known gaps across
all layers (spec, implementation, strategy), prioritised by phase.

---

## Agent Loop

| File | Covers |
|------|--------|
| `agent-loop/core-loop.md` | ReAct iteration, history push, max-turns, idle/done/busy states |
| `agent-loop/tool-dispatch.md` | Parallel dispatch, ordering, results aggregation, duplicate detection |
| `agent-loop/mode-system.md` | read/write/yolo, PolicyStore, AlwaysAllow/AlwaysDeny, policy_approval toggle, session grants |
| `agent-loop/error-recovery.md` | FailStreak, loop detection, recovery steering, empty-turn fallback |
| `agent-loop/prompt-assembly.md` | Injected prompt blocks: head/tail placement, `prompt_priority`, the after-the-gate rule, and why core blocks aren't registry entries |

## Tools

| File | Covers |
|------|--------|
| `tools/read-tool.md` | Read: path resolution, confinement, pagination, error envelope |
| `tools/write-tool.md` | Write: overwrite safety, content validation, sequential edits (edit functionality is part of WriteTool, no separate edit tool) |
| `tools/bash-tool.md` | Bash: timeout, output cap, approval gate, cancellation, signal handling |
| `tools/search-tool.md` | Search: mode dispatch (grep/semantic), result formatting |
| `tools/process-tools.md` | ProcessStart/Read/Stop: lifecycle, buffering, cancel |

## TUI, Layout & Rendering

| File | Covers |
|------|--------|
| `tui/architecture.md` | TUI internal layers (L1 domain / L2 application / L3 ports / L4 adapters / L5 composition), port inventory, unidirectional dataflow, isolation rules |
| `tui/layout-engine.md` | Panel hierarchy, flex split, resize constraints, minimum sizes |
| `tui/event-loop.md` | 20 fps loop, AgentEvent queue drain, ncurses getch/timeout |
| `tui/scroll-system.md` | Scrollback buffer, viewport, page-up/down, search-in-scroll, cursor tracking |

## TUI, Input System

| File | Covers |
|------|--------|
| `tui/input-system/slash-engine.md` | BitchX-style drawer (narrows per depth), `?` at any depth opens full help page, Cisco IOS `/no` prefix negation, partial command acceptance, Tab accepts autosuggest shadow + inline cycling, Ctrl-D/R, readline input editing, BitchX/IrcII shortcut aliases (`/q`=quit, `/sl`=session list, `/sy`=system). Current exact-only dispatch is called out; SE-17..SE-26 are 0.6.0 acceptance criteria. |
| `tui/input-system/auto-complete.md` | Per-depth completion, zsh-style inline menu cycling, Ctrl-D list-choices, Ctrl-R reverse-i-search, value cycling, Up/Down synced with cycle, alias-aware shadow |
| `tui/input-system/nested-commands.md` | Historical tree/key-chain model. Its old flat alias table is superseded by `tui/command-shortcuts.md`. |
| `tui/input-system/contextual-help.md` | `?` at any depth replaces `/help`. Scales to infinite key-chain nesting. Full info_dialog: description, args, types, choices, ranges, current values, flags, related commands. `?` stripped on dismiss. |
| `tui/command-namespace.md` | 0.6.0 command grammar, domain roots, standard verbs, `/get`/`/set` property plane, `/work`/`/window` boundaries, plugin placement (settings first, `/plugin/<id>`, no plugin roots), migration table |
| `tui/command-shortcuts.md` | `short`/`aliases` data model, derived/overridden path tokens, global conflict index, exact/alias/glued/prefix resolution, ambiguous drawer, completion/help UX, acceptance tests |
| `tui/work-surfaces.md` | Resource model for sessions/jobs/agents/watches, status cells `[s][j][a][w]`, host Work board, resource viewer windows, job/subagent metadata, lifecycle matrix, PTY tests |
| `tui/window-projects.md` | Per-window project roots and project sessions: project vs workspace axis, per-project stores/config/prompts/worktrees, `/project open`, security, restore and tests |
| `tui/command-tree.md` | The JSON mapper contract: `completions.json` is the only command surface, code binds behaviour only, documented fields (including `short`) are immutable, merges concatenate children and count collisions, plugins mount under `/plugin/<id>` and cannot add roots |
| `tui/headless-one-shot.md` | Deferred: `amber --prompt ... --exit-after-turn [--ephemeral]`, same Tui/loop/teardown, PTY in CI, ephemeral XDG roots, no in-app scripting in v1 |

## TUI, Widgets

| File | Covers |
|------|--------|
| `tui/dialogs.md` | Modal dialog stack, promise-based agent-thread blocking |
| `tui/settings-ui.md` | Settings panels, CRUD list management, provider config |
| `tui/menu-select.md` | Menu selection widget: scroll, search, confirm, color |

## Display / Markdown

| File | Covers |
|------|--------|
| `display/markdown-parser.md` | md4c pipeline, block/span callbacks, RichLine emission |
| `display/table-rendering.md` | Column width, borders, alignment, cell wrapping, header separator |
| `display/ansi-parsing.md` | SGR code parsing, Run style mapping, strip vs render |
| `display/icon-rendering.md` | Nerd Font icon mapping, fallback, alignment, ASCII mode |

## Config

| File | Covers |
|------|--------|
| `config/file-config.md` | `amber.conf`: format, sections, key resolution, defaults |
| `config/cli-config.md` | CLI flags: `--model`, `--yes`, `--no-stream`, priority |
| `config/ui-config.md` | Runtime config via TUI commands, save/load |
| `config/merge-semantics.md` | Priority: CLI > UI > file, partial override, unknown-key handling |

## LLM Client

| File | Covers |
|------|--------|
| `llm-client/dialect.md` | Provider wire-protocol port: flavor registry + fallback, request/response translation, streaming decode, usage mapping, error classification per protocol; adding a provider is one dialect file + one registry row |
| `llm-client/streaming.md` | SSE parser, token events, cancel during stream, buffering |
| `llm-client/agent-loop-reliability.md` | Chat port + scripted fake for hermetic loop tests, typed retry policy (backoff/cancel), n_ctx fallback budget; context immutability is non-negotiable |
| `llm-client/rate-governor.md` | Provider-scoped flow control: cold start, Retry-After pause vs AIMD interval/concurrency, published token/request budgets, fast recovery, provider isolation, `wait`/`throttle` tools |
| `llm-client/http-transport.md` | libcurl setup, retry, timeout, cancel-check callback |
| `llm-client/model-probe.md` | `/v1/models`, capability detection, fallback chain |
| `llm-client/error-handling.md` | HTTP 4xx/5xx, JSON parse errors, malformed response recovery |

## Compression

| File | Covers |
|------|--------|
| `compression/compression-pipeline.md` | End-to-end: gate → live-prefix replay → classify/extract → assemble → free pass → budgets → invariant |
| `compression/loop-collapse.md` | Consecutive tool-call merging, applied to the assembled output |
| `compression/turn-classification.md` | Index space, tag application, protected tail, archive and summary |

## Context

| File | Covers |
|------|--------|
| `context/context-ownership-and-parallel-compression.md` | Inviolable deque/immutability contract, single-owner-thread model, event-driven progress, live-prefix KV reuse |

## Skills

| File | Covers |
|------|--------|
| `skills/agent-skills.md` | Umbrella: two-tier model (authored vs learned), scope axes, precedence, progressive disclosure, injection-slot policy, budgets |
| `skills/skill-files.md` | Authored `SKILL.md` format (Agent Skills open standard subset), tolerant frontmatter parser, directory scanner, interop gate |
| `skills/skill-catalog.md` | `SkillCatalog`/`SkillOverrides` ports, `read_skill`/`write_skill`/`list_skills` tools, `/set skills` + `/get skills` curation commands |
| `skills/skill-acquisition.md` | Skills Explorer: `skills.sh` index adapters, search/browse/facets, install sheet with review gate, atomic tree install, provenance ledger, updates/diff, trust and token-cost surfacing, `/skills` command + CLI parity |

## MCP (Model Context Protocol)

| File | Covers |
|------|--------|
| `mcp/mcp-architecture.md` | Umbrella: amber as MCP client, scope (tools/resources/prompts, no roots/sampling), primitive mapping, session lifecycle, invariants |
| `mcp/mcp-transport.md` | JSON-RPC 2.0 wire contract, stdio (spawn/framing/shutdown), Streamable HTTP (POST/SSE/session id), timeouts, cancellation |
| `mcp/mcp-client.md` | Per-server client session: initialize/negotiation, discovery, pagination, listChanged, tool/resource/prompt adapters, server config |
| `mcp/mcp-ui.md` | `/mcp` + `/prompt` commands (legacy roots; migrate to `/plugin mcp` and `/get plugin settings mcp` / `/set plugin settings mcp`), dynamic prompt subtree, status bar, CLI flags |
| `mcp/mcp-security.md` | Trust model: untrusted-by-default servers, approval gate, read-mode policy, output caps, cancellation, no server-initiated capabilities |

## Plugins

| File | Covers |
|------|--------|
| `plugins/plugin-framework.md` | Harness extension engine (design record, implemented through PF-4): three-mechanism model (registries/events/host services), typed capabilities + ledger, typed events over `EventBus` with fire sites and performance invariants, prompt blocks, status segments, panels, provider/wallet/allowance capabilities, threading contract, deferred register |
| `plugins/watch-observer.md` | Background watch + sentry: standalone `plugins/watch` architecture, watch/observer/worker resource classes, one-shot read-only observer, PR failure reports, prompt assembly, host delivery seam, single-GPU policy. Current `/watch` command root migrates to `/plugin watch` under the 0.6.0 namespace contract. |
| `plugins/developer-guide.md` | How to build amber plugins: quickstart, tier decision tree, availability table (the single status surface), core plugin anatomy, capability catalogue with code, typed event catalogue, provider path, external quick start, review checklist |
| `plugins/README.md` | External plugin protocol (shipping): JSON-RPC wire contract, manifest.json, `PluginManager`, tool registration, `/get`/`/set plugin` administration, shipped external plugins (sysinfo, cdp) |
| `../plugin-framework-tracker.md` | Plugin framework tracker: phases PF-1..PF-6, decision log (D1..D23), deferred register, risks, definition of done |

## Memory / Experience

| File | Covers |
|------|--------|
| `memory/extraction.md` | Extraction triggers, signal detection, quality gating |
| `memory/memory-store.md` | Storage, dedup, query, recall-injection into context (project-scoped learned skills) |
| `memory/skill-operations.md` | Learned skill upsert/deprecate lifecycle (authored skills live in `skills/`) |
| `memory/learn-ui.md` | `/learn` visibility + management: show/inspect/forget/pin, `/get learn` summary, store list/remove/promote APIs |

## Session

| File | Covers |
|------|--------|
| `session/save-load.md` | Serialization format, validation, corruption handling, migration |
| `session/session-list.md` | Directory listing, metadata extraction, sort order |

## Search Backends

| File | Covers |
|------|--------|
| `search-backends/grep-backend.md` | `grep -rnI` invocation, output parsing, flag passthrough, binary skip |
| `search-backends/semantic-backend.md` | Lexical index, build, query, scoring, incremental update |
| `search-backends/backend-selection.md` | Tool-level `mode=` argument, fallback, registration lifecycle |

## Workspace / Security

| File | Covers |
|------|--------|
| `workspace/path-confinement.md` | Confine algorithm, `../` prevention, symlink handling, root resolution |
| `workspace/security-model.md` | Mode gating, tool-level restrictions, approval flow |

## Domains

| File | Covers |
|------|--------|
| `domains/coding-library.md` | The coding library's residency rule, layers, entry rules and budgets |
| `domains/project-index.md` | Amber's project index: what it records, how it is derived, staleness |
| `domains/project-instructions.md` | `AGENTS.md` injection: the 8 KB cap, git-aware nested discovery (ignore rules, worktrees), precedence |

## Git Integration

| File | Covers |
|------|--------|
| `git-integration/git-workflow.md` | Snapshots as commits, the indexed subject (`area: summary`, one area per commit), history scanning/pickaxe/bisect, the rollback ladder, the detached-window-worktree rule, and why there is no git tool |

## Benchmark / KPIs

| File | Covers |
|------|--------|
| `benchmark/MISSION.md` | The benchmark's mission and vision: why it exists, the dimensions map, the model-library strategy, definitions of done, the reference and baseline for the harness |
| `benchmark/kpi-framework.md` | Harness architecture: scenario schema, runner lifecycle, static-template engine, hermetic fake, phasing |
| `benchmark/corpus.md` | Scenario taxonomy: 9 suites, scenario ladder (agent failures, terminal, tools, prompt, coding, refactor, skills, mcp, compression), P1 scope |
| `benchmark/kpi-catalog.md` | Every measurable indicator: correctness, efficiency, robustness, resources, judgment-metric proxies |
