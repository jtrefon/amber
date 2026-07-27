# Amber Mission

**How we achieve the vision. Concrete, tactical, decision-enabling.**

The vision (`VISION.md`) says *what* we build and *why*. This document says
*how* — priorities, trade-offs, phase order, user promise, and the feature
filter that gates every decision.

---

## The user promise

- Your agent runs until it's done. Background jobs survive.
- You drive it with commands; it reports back in the scrollback.
- Every action has a command path; every command produces UI feedback.
- Help is one `?` away at any depth — you never retype the path.
- Your data stays on your machine. No telemetry, no SaaS.
- The tool gets out of your way when you know what you're doing.

---

## Priority ladder

When you can't do everything (you never can), this is the order:

```
Priority 1 — CORE LOOP     Agent runs, calls LLM, uses tools, returns answer.
                            Everything depends on this. Must be solid first.

Priority 2 — RELIABILITY    Loop detection, recovery, compression, persistence.
                            The agent must not hang, loop, or lose state.

Priority 3 — USER INTERFACE Command system, autosuggest, drawer, help, readline.
                            The user must drive the agent efficiently.

Priority 4 — TOOLING        Read, write, search, bash, process, system ops.
                            More tools = more capable agent.

Priority 5 — MANAGEMENT     Sessions, providers, config, jobs, files. CRUD.
                            The user manages the tool through the tool.

Priority 6 — LEARNING       Memory, skills, patterns. Agent improves over time.
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
| Plugin system | ❌ | Every UNIX tool is already a plugin via bash. YAGNI. |
| MCP server protocol | ❌ | Standardise on tool API, not another protocol. |
| SaaS / telemetry | ❌ | Local-first. Trust invariant. |
| Mobile app / remote web | ❌ | SSH exists. Amber lives on the server. |
| Add inline images | ⚠️ Post-1.0 | Fun but doesn't help coding/server/research. |
| Add file-path completion | ✅ P3 | Required for `/files` commands. |
| Add `?` token interception | ✅ P3 | Core UX principle. Help at any depth. |

---

## Phasing

### Phase 1 — Foundation (what works now)
- Agent loop, LLM calls, streaming
- Tool dispatch, approval gating
- Read/write/search/bash tools
- Basic TUI: scrollback, input, status bar, drawer
- Session save/load
- Config file + env vars

### Phase 2 — UX Overhaul (next)
- Command tree with `CommandNode`/`ArgSpec`/`FlagSpec`
- Autosuggest shadow (always-visible completion)
- Tab inline cycling (zsh-style)
- Ctrl-D list-choices, Ctrl-R history search
- Readline key bindings (Ctrl-A/E/B/F/W/U/K/Y/L/_, Alt-B/F/D)
- `?` at any depth (typed `?` → help page)
- Cisco IOS `/no` prefix, partial command acceptance
- BitchX/IrcII shortcut aliases (`/q`, `/sl`, `/ss`)
- Wire standalone `completion/` library into TUI palette
- `/session` CRUD (list, save, load, delete, rename)
- `/provider` CRUD (list, add, edit, delete, test)
- `/model` management (list, set, probe)
- `/job` management (list, start, read, kill)
- `/files` browsing (ls, tree, open, find)
- `/system` operations (exec, delete, rmdir, mkdir, mv, cp, info, ps, kill, df, uptime)

### Phase 3 — Long Haul (future)
- `config` key-chain (unlimited depth)
- File-path completion for commands
- Background job notifications in status bar
- Memory/skill promotion UI
- Compression pipeline visibility in TUI
- Multi-agent orchestration

---

## Non-negotiable rules

1. **1:1 mapping** — Every UI action has a command; every command has UI
   feedback. No silent actions. No command-only ghettos.
2. **Security is UI-only** — Approval dialogs block the agent thread and
   require explicit consent. `--yes` is the only bypass.
3. **Terminal-native** — If it requires a GUI dependency, it doesn't belong.
4. **Local-first** — No telemetry, no phoning home, no SaaS dependency.
   Configuration is a file, not a server.
5. **Insert-always** — No modal editing. The prompt is always ready for input.
6. **One `?` at any depth** — Never retype a path to get help.
   `/set detection loop ?`, not `/help set detection loop`.
7. **Every CRUD operation has both paths** — Providers, models, sessions,
   jobs, files — all manageable from the command line AND from dialogs.
8. **Phase order is binding** — P1 must be solid before P2 ships. P2 must
   be solid before P3 ships. No skipping.

---

## Gap register

Status: ✅ design resolved, 🔧 needs implementation, ❓ needs decision

| Gap | Layer | Phase | Status | Notes |
|-----|-------|-------|--------|-------|
| No `CommandNode` tree model | Implementation | P2 | 🔧 | Wire standalone `completion/` library |
| No autosuggest shadow rendering | Implementation | P2 | 🔧 | Use existing gray `A_DIM` colour pair |
| No `?` token interception | Implementation | P2 | 🔧 | Dual mode: no-space=inline popup, space=full page |
| No readline key bindings | Implementation | P2 | 🔧 | Ctrl-A/E/B/F/W/U/K/Y/L/T/_, Alt-B/F/D |
| No Ctrl-D / Ctrl-R | Implementation | P2 | 🔧 | Expected shell features |
| No shortcut alias resolution | Implementation | P2 | 🔧 | BitchX/IrcII scheme, alias table in nested-commands |
| Standalone `completion/` library not wired | Implementation | P2 | 🔧 | Exists, not connected to TUI |
| `/session` CRUD incomplete | Impl | P2 | 🔧 | Missing delete, rename |
| `/provider` CRUD incomplete | Impl | P2 | 🔧 | Missing list, add, edit, delete, test |
| `/model` commands incomplete | Impl | P2 | 🔧 | Missing list, probe |
| `/job` management incomplete | Impl | P2 | 🔧 | Missing list, start, kill |
| `/files` browsing missing | Impl | P2 | 🔧 | ls, tree, open, find |
| `/system` ops missing | Impl | P2 | 🔧 | exec, delete, rmdir, mkdir, mv, cp, info, ps, kill, df, uptime. `exec` can inject output into agent context. |
| No file-path completion | Implementation | P2 | 🔧 | Standard shell behaviour (bash/zsh). Spec'd AC-20. |
| Up/Down arbitration | Implementation | P2 | ✅ | zsh rule: empty=history, has matches=cycle, no matches=history-filtered |
| `config` key-chain not implemented | Implementation | P3 | 🔧 | Unlimited depth config |
| Memory/skill UI not wired | Implementation | P3 | 🔧 | No visibility into learning |
| Compression pipeline not visible in TUI | Implementation | P3 | 🔧 | Progress not shown |
| Multi-agent orchestration | Strategy | P3 | ❓ | Future, no design yet |
| `system exec` context enrichment | Design | P2 | ❓ | Inject captured output into agent conversation. No stdin (v1). |
| File `open` as embedded TUI window | Design | P2 | ❓ | Scrollable window with Tab focus, not external pager. Supports context injection. |

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
