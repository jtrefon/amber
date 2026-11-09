# Amber Mission

**How we achieve the vision. Concrete, tactical, decision-enabling.**

The vision (`VISION.md`) says *what* we build and *why*. This document says
*how*, priorities, trade-offs, phase order, user promise, and the feature
filter that gates every decision.

---

## The user promise

- Your agent runs until it's done. Background jobs survive.
- You drive it with commands; it reports back in the scrollback.
- Every action has a command path; every command produces UI feedback.
- Help is one `?` away at any depth, you never retype the path.
- Your data stays on your machine. No telemetry, no SaaS.
- The tool gets out of your way when you know what you're doing.

---

## Priority ladder

When you can't do everything (you never can), this is the order:

```
Priority 1, CORE LOOP     Agent runs, calls LLM, uses tools, returns answer.
                            Everything depends on this. Must be solid first.

Priority 2, RELIABILITY    Loop detection, recovery, compression, persistence.
                            The agent must not hang, loop, or lose state.

Priority 3, USER INTERFACE Command system, autosuggest, drawer, help, readline.
                            The user must drive the agent efficiently.

Priority 4, TOOLING        Read, write, search, bash, process, system ops.
                            More tools = more capable agent.

Priority 5, MANAGEMENT     Sessions, providers, config, jobs, files. CRUD.
                            The user manages the tool through the tool.

Priority 6, LEARNING       Memory, skills, patterns. Agent improves over time.
                            High value, depends on everything else being stable.
```

A feature at P5 does not block a feature at P2. But if a P2 feature and a P5
feature compete for the same cycle, P2 wins every time.

---

## Feature filter

| Feature request | Decision | Why |
|----------------|----------|-----|
| Add `/system exec` | ✅ P4 | Server management is core mission. UNIX tool integration. |
| Add `/files ls\|tree\|open\|find` | ✅ P3 | File browsing is fundamental UX. Every terminal needs it. |
| Add readline key bindings | ✅ P3 | Standard shell UX. Low cost, high value. |
| Add Ctrl-R history search | ✅ P3 | Every shell has it. Expected. |
| Add shortcut aliases (`/q`, `/sl`) | ✅ P3 | BitchX-inspired. Muscle memory. Low cost. |
| Add autosuggest shadow | ✅ P3 | Core UX differentiator. Anticipate, don't wait. |
| Background job notifications | ✅ P4 | Long-haul mission requires async feedback. |
| Add provider CRUD | ✅ P5 | Manage the tool through the tool. |
| Add session CRUD | ✅ P5 | Persistence is a promise. |
| Wire standalone `completion/` library | ✅ P3 | Already exists, just not wired. Free win. |
| Support Windows | ❌ | Linux is the armour. Wine/Cygwin aren't Linux. |
| Add web / GUI | ❌ | Terminal is the API. SSH exists. |
| Vim modal mode | ❌ | Prompt is insert-always. |
| Plugin system | ⚠️ Revised 2026-09-10 | The old YAGNI verdict conflated two different things. (a) A **loader for arbitrary third-party code**: still ❌: bash and MCP servers already carry that, and no runtime loading is being built. (b) An **in-process extension framework** so amber's own extension points (LLM providers, prompt blocks, status segments, panels, wallets, allowances) are *contributed* through typed registries instead of edited in shared core files, ✅, because adding a provider vendor currently means editing four core files and `/provider test` is wrong for every non-OpenAI provider. Compiled-in only, no third-party code path. See `docs/spec/plugins/plugin-framework.md`, tracker PF-1..PF-6. |
| Authored skill packages (`SKILL.md`) | ✅ P3 | Agent Skills open standard; progressive disclosure. Distinct from plugins: skills are *instructions*, not *code*. |
| MCP client (Model Context Protocol) | ✅ P6 | Revisited 2026-07-31: MCP is now the ecosystem standard for third-party capability servers (the Agent Skills standard interoperates with it). Amber stays hexagonal, MCP is an *adapter over the existing `Tool` port*, not a second tool API. Scope: client-only (tools/resources/prompts), no roots/sampling, untrusted-by-default servers behind the approval gate. See `docs/spec/mcp/` and `docs/mcp-tracker.md`. The "no plugin loader" decision stands: MCP servers are external processes, not amber plugins. |
| SaaS / telemetry | ❌ | Local-first. Trust invariant. |
| Mobile app / remote web | ❌ | SSH exists. Amber lives on the server. |
| Add inline images | ⚠️ Post-1.0 | Fun but doesn't help coding/server/research. |
| Add file-path completion | ✅ P3 | Required for `/files` commands. |
| Add `?` token interception | ✅ P3 | Core UX principle. Help at any depth. |

---

## Phasing

### Phase 1, Foundation (shipped)
The agent loop, streaming LLM client, tool dispatch with approval gating,
read/write/search/bash/process tools, session save/load, config file + env
resolution, and the terminal UI's scrollback, input and status bar are in
`main` and covered by the suite.

### Phase 2, UX and reliability (shipped foundation, active work)
The command tree (`completions.json`), autosuggest shadow, readline editing,
`?`-at-depth, aliases, all CRUD namespaces (`/session`, `/provider`, `/model`,
`/job`, `/files`, `/system`), the plugin framework (PF-1..PF-4, providers,
tools, prompts, status segments, panels, wallets), MCP, skills and memory are
shipped. The active work is the full ncurses shell (#512), asynchronous
background work (#366), input ergonomics (#445), profiles (#440), git
checkpoints (#441), and the 0.8 live benchmark gate (#371).

### Phase 3, Long haul
Release gates that measure the product (the live benchmark, patch coverage),
multi-agent orchestration beyond detached workers, and the shell hardening
that follows the component migration. These are tracked as issues, not as a
second roadmap.

---

## Non-negotiable rules

1. **1:1 mapping**: Every UI action has a command; every command has UI
   feedback. No silent actions. No command-only ghettos.
2. **Security is UI-only**: Approval dialogs block the agent thread and
   require explicit consent. `--yes` is the only bypass.
3. **Terminal-native**: If it requires a GUI dependency, it doesn't belong.
4. **Local-first**: No telemetry, no phoning home, no SaaS dependency.
   Configuration is a file, not a server.
5. **Insert-always**: No modal editing. The prompt is always ready for input.
6. **One `?` at any depth**: Never retype a path to get help.
   `/set detection loop ?`, not `/help set detection loop`.
7. **Every CRUD operation has both paths**: Providers, models, sessions,
   jobs, files, all manageable from the command line AND from dialogs.
8. **Phase order is binding**: P1 must be solid before P2 ships. P2 must
   be solid before P3 ships. No skipping.

---

## Gap register

Status: ✅ shipped, 🔧 open, ❓ needs a decision. The GitHub issue tracker is the
only open-work register; every open row names the issue that tracks it. A
capability that ships moves to the shipped table rather than being deleted, so
the history stays honest.

### Shipped

| Capability | Evidence |
|------------|----------|
| Command tree, `?` at depth, aliases, autosuggest | `completions.json` + `SettingRegistry`; `tui/command-tree.md`; pty coverage in `tests/tui_pty_test.cpp` |
| Readline editing (Ctrl-A/E/B/F/W/U/K/Y/T, Ctrl-D, kill/yank) | `tui/command_line.cpp`; `CommandLine` history and bracket-paste decoding |
| `/session` CRUD (new/list/load/save/rename/remove/fork/compress) | `completions.json`; `lib/session.cpp`; session browser |
| `/provider` list/test/delete + configuration | provider plugins; `lib/providers_repo_files.cpp`; `/get`/`/set provider` |
| `/model` switch, list and probe | model feed under `/set model`; `lib/model_probe.cpp`; `/get model list` |
| `/job` list/start/read/stop | `lib/job.cpp`; `completions.json`; job feed |
| `/files` ls/tree/open/find | `completions.json`; resource-viewer windows |
| `/system` exec/CRUD/process/disk ops | `completions.json`; `os.system.*` handlers; job-backed async execution |
| Config key-chain (`/get`/`/set`, unlimited depth) | `SettingRegistry`; `docs/spec/tui/command-tree.md` |
| Memory and skills UI | `/learn`, `/get learn`, `/skills`; `docs/spec/memory/learn-ui.md`, `docs/spec/skills/` |
| Skills acquisition (explorer, install gate, updates, CLI) | #413, #414, #449, #454, #457, #458, #460; `docs/skills-acquisition.md` |
| Hermetic agent-loop tests with a scripted LLM port | `tests/agent_loop_test.cpp`; `docs/spec/llm-client/agent-loop-reliability.md` |
| MCP client (tools/resources/prompts, HTTP + stdio) | `plugins/mcp/`; `docs/spec/mcp/`; activation is off the startup path (#368) |
| Plugin framework: capabilities, ledger, typed events, providers, status/panels/wallets | PF-1..PF-4; `docs/spec/plugins/plugin-framework.md`; `docs/plugin-framework-tracker.md` |
| Per-project state and legacy adoption | `agent::config_paths()`; `docs/spec/memory/memory-store.md` |
| Compression progress visible while it runs | `Window::compressing`; the activity verb in `tui/render_engine.cpp` |
| Embedded file open (resource window, not a pager) | `/files open`; `tui/` resource viewer |

### Open

| Gap | Layer | Phase | Status | Tracking |
|-----|-------|-------|--------|----------|
| Ctrl-R reverse history search is still a stub | Implementation | P2 | 🔧 | #445 (input ergonomics) |
| File-path Tab completion (bash/zsh standard) | Implementation | P2 | 🔧 | #445 |
| Async background work: one task inventory, delivery/wake, persistence | Implementation | P3 | 🔧 | #366, #375-#383 |
| Full ncurses component shell (cell surface, themes, screen migration) | Implementation | P3 | 🔧 | #512, #513-#519 |
| Profiles and git-integrated checkpoints | Strategy | P3 | ❓ | #440, #441 |
| 0.8 live benchmark baseline and a release gate | Implementation | P2 | 🔧 | #371 |
| Multi-agent orchestration beyond `delegate` | Strategy | P3 | ❓ | #379 (detached workers) |

---

## Measuring success

| Metric | Target | When |
|--------|--------|------|
| Agent runs 24h without hang or loop | ✅ | P1 |
| All CRUD operations work via command AND UI | ✅ | P2 |
| User can learn 80% of commands in one session | ✅ | P2 (autosuggest + help at depth) |
| `?` without space shows inline options popup | ✅ | P2 |
| `?` with space shows full help page for resolved path | ✅ | P2 |
| Up/Down arbitration matches zsh rule (empty→history, has matches→cycle) | ✅ | P2 |
| File-path Tab completion follows bash/zsh standard | ✅ | P2 |
| Background jobs survive agent restart | ✅ | P3 |
| Memory recall meaningfully improves answers | ✅ | P3 |
| Authored skills discoverable + activatable without prompt bloat | ✅ | P3 |
| Learned skills stay project-scoped (no cross-project leak) | ✅ | P3 |
