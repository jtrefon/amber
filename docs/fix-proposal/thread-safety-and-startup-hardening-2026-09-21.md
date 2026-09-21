# Thread Safety and Startup Hardening — Proposal

- **Status:** 🟡 Awaiting sign-off
- **Date:** 2026-09-21
- **Register:** `docs/issues.md` (T1..T5)
- **Proposed FIX ids:** FIX-035 (T1 + TSan), FIX-036 (T2), FIX-037 (T3), FIX-038 (T4), FIX-039 (T5)
- **Branch:** `fix/agent-config-thread-safety` (phase 1), one branch per FIX thereafter

Findings below come from a full-tree review of `main` at `bf98d4d`, verified against
source. Where a claim is measurable it was measured on this machine; measurements
are stated as such. This proposal covers only what is listed; see **Not in scope**.

---

## Problem

### T1 — 🔴 Critical: `Agent::cfg_` is read by the UI every frame while workers write it

`Config cfg_` is a plain member (`include/agent/agent.h:377`) handed out as a
`const Config&` (`include/agent/agent.h:206`). There is no lock, no atomic, and no
snapshot:

| Direction | Site | When |
|---|---|---|
| UI reads | `tui/render_engine.cpp:213-216` (`model`, `reasoning_effort`, `mode`) | **every render** |
| UI reads | `tui/tui_input.cpp:2748,2797` (`mode`, `thinking`) | feeds |
| UI reads | `tui/tui_session.cpp:74,82` (`model`, `context_size`) | session snapshot |
| Worker writes | `lib/agent.cpp:930` — inside `Agent::run()`: `apply_server_autodetect(cfg_)` | cold start, model empty |
| UI writes | `lib/agent.cpp:96-97` `set_model` | catalog callback, provider switch |

Concrete trigger: cold start with an empty model and no warm catalog. The user
sends a prompt; the worker enters `Agent::run()` and resolves the model into
`cfg_.model` while the UI thread renders the status bar from `config().model`.
The background catalog refresh then lands on the UI thread and runs
`tui/tui_input.cpp:2336`, which calls `w->agent->set_model(...)` for every window
whose model is still empty — **without** the `runs_.busy()` guard that `/set model`
has (`tui_input.cpp:1321`). Two threads then write, and a third reads, the same
`std::string`s. That is undefined behaviour: torn reads, and reads of a buffer
freed mid-assignment.

Attribution: `#142` introduced the worker-side write and the unguarded UI loop;
FIX-034 phase 1 fixed `Window::dirty`, `PromptRegistry` and the save-path
quiescence guards, but not this. The architecture's own rule — "thread safety is
by single ownership, not locking" (`AGENTS.md`) — is violated: the UI is reading
state it does not own.

**Why the existing gates did not catch it:** CI runs ASan+UBSan only
(`.github/workflows/ci.yml:199-223`). Neither detects data races; only
ThreadSanitizer does, and there is no TSan job.

### T2 — 🟠 High: UI-thread blocking remains in three paths

The class removed from startup in `#142` is still present elsewhere:

| Path | Evidence | Cost |
|---|---|---|
| `/system exec` | `tui/tui_input.cpp:1888-1893` — polls `sleep(50ms)` up to 600 times on the UI thread | **UI frozen up to 30 s** |
| `git_refresh()` | `tui/render_engine.cpp:696` runs 2 × `popen`; called at startup *before the first paint* (`tui/tui.cpp:381`) and after **every tool result** (`tui/event_router.cpp:423`) | measured ~65 ms here; `git status` on a large repo is far worse, per tool call |
| `ps` / `df` / `uptime` / `uname` | `tui/tui_input.cpp:2007-2064` — raw `popen` on the UI thread | tens of ms, unbounded on a loaded host |

The `git_refresh` case contradicts the stated trademark directly: startup work is
supposed to be local-only and bounded, and here the UI forks two processes before
it has painted once.

### T3 — 🟠 High (security): secret-bearing files are world-readable

Measured on this machine, permissions only (no content read):

```
~/.config/amber/providers/kilocode.conf   -rw-r--r--   (holds api_key=…)
~/.config/amber/config                    -rw-r--r--   (may hold api_key)
.amber/sessions/1788683440839-0.json      -rw-r--r--   (full conversation transcript)
.amber/logs/*.jsonl                       -rw-r--r--   (full conversation transcript)
.amber/settings, .amber/policy.json       -rw-r--r--
```

No writer sets a restrictive mode: `lib/providers_repo_files.cpp:89`,
`lib/config.cpp:206`, `lib/session.cpp` (`save`, atomic tmp+rename), and
`lib/conversation_log.cpp:30` all inherit the process umask (typically 0644). Any
local user, backup, or file-sync agent can read the LLM API key (billing and
quota attached) and the entire conversation history.

**The codebase already knows the right answer:** `lib/mcp_config.cpp:232` sets
`fs::perms::owner_read | owner_write` for MCP server configs. That precedent was
simply never applied to the files that matter most.

### T4 — 🟡 Medium: local logs grow without bound

`lib/agent.cpp:613-618` opens `<workspace>/.amber/logs/{ts}.jsonl` — one file per
`Agent` — and nothing ever prunes them: grep for
`prune|retention|rotate|max_age|keep_last` across `lib/ tui/ src/ tools/` finds
only unrelated skill-install cleanups. Measured in this project:
**11,910 files, 214 MB**. Sessions have a store with an index; logs have nothing.

### T5 — 🟡 Medium: MCP auto-connect blocks startup before first paint

`tui/tui.cpp:140` calls `mcp_servers_.connect_all()` on the startup path — a
blocking handshake per enabled + `auto_connect` server
(`lib/mcp_config.cpp:258-260`). Inert for the current config (no MCP servers);
a startup blocker for anyone who configures one. Same shape as the catalog fetch
that caused the 20 s startup.

---

## Target state

### T1 — publish config; never share it

`Config` becomes immutable once published. Writers do copy-modify-publish;
readers take a snapshot handle for the duration of their use.

```cpp
// include/agent/agent.h
const Config& config() const;                     // removed from UI call sites
std::shared_ptr<const Config> config_snapshot() const;  // lock-free read
```

- Writers (`set_model`, worker-side autodetect) clone, mutate, publish
  (`std::atomic_store` on a `shared_ptr` — C++17, no mutex on the read path).
- The UI holds the snapshot while rendering a frame; the reference can no longer
  dangle and no `std::string` is read while another thread writes it.
- `tui/tui_input.cpp:2336` gains the same `runs_.busy()` guard `/set model` has,
  and the model is adopted through the event path rather than a direct write.
- `Agent::run()`'s cold-start resolution stays where it is (the worker must be
  able to resolve a model), but its result is published, not scribbled.

Rejected alternatives — recorded so the decision is not re-litigated:

- **Mutex around `cfg_`**: `render_engine` reads three fields per frame; a
  lock-and-copy of a fat `Config` per frame is allocation churn on the hot path.
- **Remove worker-side resolution entirely**: cold start with an unreachable
  endpoint then blocks the *UI* thread on the unavoidable network call — the
  regression `#142` was written to fix.

### T1 — TSan job (the standing guard)

Mirror the existing sanitizer job (`.github/workflows/ci.yml:199-223`): clang++,
`-fsanitize=thread`, `make test`, plus `tui_pty_test`. This is the **Red** for T1:
the race must be reproduced and captured before it is fixed.

### T2 — no I/O on the UI thread

- `/system exec`: start the job, return immediately with `job started (id)`, and
  append the captured output to the scrollback when it completes — the job
  service already tracks, caps and can kill it (`tui_input.cpp:1885`).
- `git_refresh()`: refresh on a detached worker into a cached snapshot; the
  renderer reads the cache. Removes it from startup and from the per-tool-result
  path (`tui/event_router.cpp:423`).
- `ps`/`df`/`uptime`/`uname`: same treatment — these are exactly the
  "background work posted through `post_to_ui_thread`" pattern the TUI already
  uses for the model catalog.

### T3 — owner-only permissions for amber-owned data

- New writes create secret/transcript files with `owner_read | owner_write`
  (0600) — following `lib/mcp_config.cpp:232`, not inventing a second convention.
- Existing files are tightened on the next write of that file (no silent mass
  chmod of a user's tree).
- Covered: provider configs, `config`, session saves, conversation logs.
  `.amber/settings` and `policy.json` hold no secrets but are cheap to include.

### T4 — retention policy for logs

Prune on open (cheap, once per agent) against a policy; keep the newest N per
workspace and drop files older than a horizon. Exact policy is **decision D4**.

### T5 — connect MCP servers after first paint

Connect on a detached worker after the first frame; post server state when it
arrives. The UI already has the machinery (`post_to_ui_thread`).

---

## Red artifacts (one per FIX, committed before the fix)

| FIX | Red |
|---|---|
| FIX-035 | TSan job + a deterministic cold-start repro (pty: no cache, prompt, catalog refresh lands mid-run) that reports the `cfg_` race |
| FIX-036 | pty test: `/system exec sleep 30` — the UI must still accept input and repaint within a bounded time; startup guard: first paint must not fork git |
| FIX-037 | unit test: files created for provider config, session save and log open are mode 0600 (`stat`) |
| FIX-038 | unit test: N logs with old mtimes prune to the policy |
| FIX-039 | pty test: with a configured MCP server that is slow to handshake, first paint still occurs under the startup bound |

## Landing order

1. **FIX-035** (T1 + TSan) — crash potential, and TSan tells us whether T1 is the
   only race or merely the first one it finds. Branch `fix/agent-config-thread-safety`.
2. **FIX-036** (T2) — startup is the trademark; `git_refresh` at startup is the
   direct hit.
3. **FIX-037** (T3) — small diff, real security value, precedent already in-tree.
4. **FIX-038** (T4) — needs decision D4.
5. **FIX-039** (T5) — lowest trigger probability (requires configured MCP servers).

## Decisions requested (sign-off)

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Config hand-off mechanism | (a) copy-on-write `shared_ptr` snapshot (b) mutex + snapshot copy (c) remove worker-side resolution | **(a)** |
| D2 | TSan job | (a) gating, full `make test` + pty (b) gating, targeted subset (c) informational like `lint-full` | **(a)**, falling back to (b) if runtime is unacceptable |
| D3 | Permission scope | (a) keys + transcripts + settings (b) keys only | **(a)** |
| D4 | Log retention policy | (a) newest N per workspace (b) age horizon (c) both (d) size cap | **(c)**, with the existing 214 MB left in place (no destructive sweep) |
| D5 | `/system exec` UX | (a) async, output appended on completion (b) keep synchronous freeze | **(a)** |
| D6 | `git_refresh` | (a) detached worker + cached snapshot (b) drop from startup only | **(a)** |

## Risk

| FIX | Risk | Mitigation |
|---|---|---|
| FIX-035 | Touching every `agent->config()` reader; snapshot lifetime bugs | Compiler-enforced: `config()` returning a reference is removed, so every reader must be revisited; TSan job proves it |
| FIX-036 | `/system exec` becomes asynchronous — muscle memory changes | Status line states the job id and completion appends output; `/jobs` already lists it |
| FIX-037 | A user's existing files keep old modes until rewritten | Stated explicitly; no silent mass chmod |
| FIX-038 | Pruning deletes data a user wanted | Default horizon is conservative and configurable; nothing is deleted during the rollout |
| FIX-039 | MCP tools briefly unavailable at startup | Status segment reports "connecting"; tools appear when ready |

## Verification

`make clean && make && make test && make lint && make analyze`, plus the CI
matrix (g++, clang++, macOS, ASan+UBSan, **TSan**), `make check`, `make duplicates`,
`make format-check-changed`. Each FIX lands its Red first and its Green on the same
branch, with the proposal updated in the same PR (repo convention).

## Not in scope

- The modal keypad contract (fixed in `#144`).
- The `q`-closes inconsistency across modals (cosmetic).
- `tui/tui_input.cpp` at 2,833 lines (real structural debt; needs its own proposal).
- FIX-034 phase 2 (cross-thread agent state) — owned by the existing workstream.
