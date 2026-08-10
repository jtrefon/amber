# Spec Directory

Central contracts for all amber features. Every file here defines *what must
hold* for a feature to be correct. If the code violates the spec, the code is
wrong.

## Reading order

| Step | Document | Purpose |
|------|----------|---------|
| 1 | `VISION.md` | **Why** — inspiration, north star, the spirit. Never changes. |
| 2 | `MISSION.md` | **What and when** — priority ladder, feature filter, phasing, gap register, user promise. |
| 3 | `ux-policy.md` | **How the interface works** — 1:1 mapping, autosuggest, `?`-at-depth, readline, widget policy, habit loops. |
| 4 | Individual specs | **Contracts** — invariants, scenarios, error states, cross-references. |

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

## Tools

| File | Covers |
|------|--------|
| `tools/read-tool.md` | Read: path resolution, confinement, pagination, error envelope |
| `tools/write-tool.md` | Write: overwrite safety, content validation, sequential edits (edit functionality is part of WriteTool — no separate edit tool) |
| `tools/bash-tool.md` | Bash: timeout, output cap, approval gate, cancellation, signal handling |
| `tools/search-tool.md` | Search: mode dispatch (grep/semantic), result formatting |
| `tools/process-tools.md` | ProcessStart/Read/Stop: lifecycle, buffering, cancel |

## TUI — Layout & Rendering

| File | Covers |
|------|--------|
| `tui/layout-engine.md` | Panel hierarchy, flex split, resize constraints, minimum sizes |
| `tui/event-loop.md` | 20 fps loop, AgentEvent queue drain, ncurses getch/timeout |
| `tui/scroll-system.md` | Scrollback buffer, viewport, page-up/down, search-in-scroll, cursor tracking |

## TUI — Input System

| File | Covers |
|------|--------|
| `tui/input-system/slash-engine.md` | BitchX-style drawer (narrows per depth), `?` at any depth opens full help page, Cisco IOS `/no` prefix negation, partial command acceptance, Tab accepts autosuggest shadow + inline cycling, Ctrl-D/R, readline input editing, BitchX/IrcII shortcut aliases (`/q`=quit, `/sl`=session list, `/sy`=system) |
| `tui/input-system/auto-complete.md` | Per-depth completion, zsh-style inline menu cycling, Ctrl-D list-choices, Ctrl-R reverse-i-search, value cycling, Up/Down synced with cycle, alias-aware shadow |
| `tui/input-system/nested-commands.md` | Full command tree: settings, provider/model/session/job CRUD, file browsing (`/files`), system operations (`/system exec|rmdir|mv|cp|ps|kill|df|uptime`), `config` key-chain. Shortcut alias scheme with design rules. Tree model: `CommandNode` with `ArgSpec`/`FlagSpec`, depth-aware walk, Cisco `/no` prefix |
| `tui/input-system/contextual-help.md` | `?` at any depth replaces `/help`. Scales to infinite key-chain nesting. Full info_dialog: description, args, types, choices, ranges, current values, flags, related commands. `?` stripped on dismiss. |

## TUI — Widgets

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
| `llm-client/streaming.md` | SSE parser, token events, cancel during stream, buffering |
| `llm-client/agent-loop-reliability.md` | Chat port + scripted fake for hermetic loop tests, typed retry policy (backoff/cancel), n_ctx fallback budget; context immutability is non-negotiable |
| `llm-client/http-transport.md` | libcurl setup, retry, timeout, cancel-check callback |
| `llm-client/model-probe.md` | `/v1/models`, capability detection, fallback chain |
| `llm-client/error-handling.md` | HTTP 4xx/5xx, JSON parse errors, malformed response recovery |

## Compression

| File | Covers |
|------|--------|
| `compression/compression-pipeline.md` | End-to-end: snapshot → collapse → LLM → parse → apply |
| `compression/loop-collapse.md` | Consecutive tool-call merging rules, lossless vs lossy |
| `compression/turn-classification.md` | Tag assignment: keep/drop/summarize per-turn logic |

## Skills

| File | Covers |
|------|--------|
| `skills/agent-skills.md` | Umbrella: two-tier model (authored vs learned), scope axes, precedence, progressive disclosure, injection-slot policy, budgets |
| `skills/skill-files.md` | Authored `SKILL.md` format (Agent Skills open standard subset), tolerant frontmatter parser, directory scanner, interop gate |
| `skills/skill-catalog.md` | `SkillCatalog`/`SkillOverrides` ports, `read_skill`/`write_skill`/`list_skills` tools, `/set skills` + `/get skills` curation commands |

## MCP (Model Context Protocol)

| File | Covers |
|------|--------|
| `mcp/mcp-architecture.md` | Umbrella: amber as MCP client, scope (tools/resources/prompts, no roots/sampling), primitive mapping, session lifecycle, invariants |
| `mcp/mcp-transport.md` | JSON-RPC 2.0 wire contract, stdio (spawn/framing/shutdown), Streamable HTTP (POST/SSE/session id), timeouts, cancellation |
| `mcp/mcp-client.md` | Per-server client session: initialize/negotiation, discovery, pagination, listChanged, tool/resource/prompt adapters, server config |
| `mcp/mcp-ui.md` | `/mcp` + `/prompt` commands, dynamic prompt subtree, `mcp.*` get/set keys, status bar, CLI flags |
| `mcp/mcp-security.md` | Trust model: untrusted-by-default servers, approval gate, read-mode policy, output caps, cancellation, no server-initiated capabilities |

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

## Git Integration

| File | Covers |
|------|--------|
| `git-integration/git-workflow.md` | Three-layer architecture (shell prompt, agent prompt, bash execution), commit/rollback workflow, invariants, tool-deferral reasoning |

## Benchmark / KPIs

| File | Covers |
|------|--------|
| `benchmark/MISSION.md` | The benchmark's mission and vision: why it exists, the dimensions map, the model-library strategy, definitions of done — the reference and baseline for the harness |
| `benchmark/kpi-framework.md` | Harness architecture: scenario schema, runner lifecycle, static-template engine, hermetic fake, phasing |
| `benchmark/corpus.md` | Scenario taxonomy: 9 suites, scenario ladder (agent failures, terminal, tools, prompt, coding, refactor, skills, mcp, compression), P1 scope |
| `benchmark/kpi-catalog.md` | Every measurable indicator: correctness, efficiency, robustness, resources, judgment-metric proxies |
