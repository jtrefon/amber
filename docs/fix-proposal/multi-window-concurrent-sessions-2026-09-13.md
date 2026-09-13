# Multi-Window Concurrent Sessions + `/session fork`

Issue: window switching and concurrent agent runs are gated by a single
process-global busy flag; windows cannot host independent, simultaneously
running sessions. (New issues.md entry to be registered with this proposal.)

## Problem

### 1. The symptom: "cannot switch windows while agent is busy"

Three independent gates, all fed by one global flag (`EventRouter::busy_`):

| Site | File | Effect |
|------|------|--------|
| `KeyBinder::dispatch` drops Alt+digit / Ctrl+N / ESC+digit when `state.busy` | `tui/key_binder.cpp:12,22,36` | keys never reach WindowOps |
| `WindowOps::switch_to` / `close_window` reject via `port_.is_busy()` | `tui/window_ops.cpp:17,34` | "cannot switch windows while agent is busy" |
| `Tui::send_async` early-returns on `router_->busy()`; Enter funnels into the single `pending_prompt_` | `tui/tui.cpp:238,684` | one agent run at a time, process-wide |

### 2. The gap: run machinery stayed single-flight

The windowing model is already multi-session:

- Each `Window` owns `std::unique_ptr<agent::Agent>` — its own `Context`,
  `Config` snapshot, compressor, gate, memory store, policy
  (`tui/window.h:32`, `tui/window_manager.cpp:18-46`).
- Every `AgentEvent` carries `window_id`; `route_event` delivers to the
  origin window and survives window erasure (`tui/event_router.h:24-33`).
- Per-window persistence exists: `autosave(Window&)`,
  `save_window_sessions()`, `WorkspaceState::windows`.
- Per-window JSONL transcripts are free: `ConversationLog` expands `{ts}`
  per agent on first turn (`lib/agent.cpp:568-576`).

What is NOT per-window:

- `EventRouter` owns **one** `std::thread`, **one** `busy_`, **one**
  `cancel_` (`tui/event_router.h:124,167-169`). One worker for N windows.
- Global status fields on `Tui` written by whichever worker is live —
  `state_`, `stats_`, `ctx_used_`, `ctx_estimate_`, `live_ctx_offset_`,
  `running_tool_`, `running_tool_desc_`, `compressing_` (`tui/tui.h:206-217`).
  Two concurrent agents would clobber each other's gauge/status.
- One global `pending_prompt_` queue (`tui/tui.h:121`).
- `cfg_.cancel_token` is a shared flag — `Config` copies share state
  (`include/agent/config.h:140-143`), every window's agent holds the same
  flag, and tools bind the host token once at registration
  (`tui/tui_main.cpp:186`). One Esc cancels everything, including another
  window's in-flight bash call.
- `Agent` ctor rebinds the shared registry's skill tools to its own
  `SkillCatalog` (`lib/agent.cpp:89-90` via `register_skill_tools`): the
  *newest* window's catalog wins, so window A's `read_skill` can resolve
  against window B's catalog. Latent cross-talk today; a correctness bug
  under concurrency.
- `send_async` calls `router_->clear()` — under concurrency it would drop a
  sibling window's queued events (`tui/tui.cpp:242`).
- `TuiWindowOpsHooks::on_switch` clears ALL `pending_tools_`, breaking the
  in-place spinner close for background windows (duplicate lines)
  (`tui/tui_window_ops_hooks.cpp:14`).
- `SessionController::snapshot` writes global `ctx_used_`/`stats_` into
  every session file (`tui/tui_session.cpp:76-83`).
- `compress_worker` shares `router_->thread()` (`tui/tui.cpp:326-337`).

### 3. Settings semantics are already incoherent for per-window use

- `/set model` broadcasts to **all** windows' agents
  (`tui/tui_input.cpp:1283-1289`).
- `/set policy.mode` writes only `tui_.cfg_.mode` and never propagates to
  existing agents (there is no `Agent::set_mode`; `render_system_prompt`
  reads the agent's own `cfg_.mode`, `lib/agent.cpp:170`). Mode changes
  silently apply only to agents created afterwards — a pre-existing bug the
  per-window work must fix, not preserve.

## Root cause

The hexagonal input-loop refactor (issue #106) split window *management*
from the event loop, but left the *run* concern — thread ownership, busy,
cancel, prompt queue — as globals on `EventRouter`. "Agent is running" is
therefore a process-global fact when the domain model says it is a
per-window fact. The fix is to move run-state to the window scope the rest
of the architecture already assumes.

## Target architecture

### Phase A — per-window run state (the bug fix)

**1. `RunRegistry` — new pure-ish domain class** (`tui/run_registry.h/.cpp`)

Owns one `RunSlot` per window, keyed by the stable `Window::id` (slots must
have stable addresses — hooks capture `RunSlot*`; use
`std::unordered_map<size_t, std::unique_ptr<RunSlot>>`):

```cpp
struct RunSlot {
    std::thread thread;                    // the window's agent worker
    std::atomic<bool> busy{false};
    std::atomic<bool> cancel{false};
    std::deque<std::string> pending;       // per-window queued prompts
    mutable std::mutex mtx;                // guards pending only
};

class RunRegistry {
public:
    RunSlot& slot(size_t window_id);                  // create-on-demand
    bool busy(size_t window_id) const;
    bool any_busy() const;                            // quit/shutdown gate
    void request_cancel(size_t window_id);
    void cancel_all();
    void join_all();                                  // shutdown path
    void enqueue(size_t window_id, std::string prompt);
    std::optional<std::string> pop_pending(size_t window_id);
    void erase(size_t window_id);                     // idle windows only
};
```

**2. EventRouter demoted to its name.** Remove `thread_`, `busy_`,
`cancel_`. It keeps: the event queue, `pending_tools_` (already keyed by
`window_id`), approval/api-key/ask queues, drain/dispatch, and
`make_hooks(window_id, RunSlot&)` — hooks capture the *slot's* cancel flag,
not a global. `shutting_down_` stays global (process teardown).

**3. `Window` gains its display-state fields.** Move `state`, `stats`,
`ctx_used`, `ctx_estimate` (atomic — written by the worker via the
context-events subscription), `live_ctx_offset`, `running_tool`,
`running_tool_desc`, `compressing` from `Tui` into `Window`. Status bar and
`snapshot(w)` read the active window's values — which also fixes the
session file capturing another window's telemetry.

**4. Gating rules after the change:**

| Operation | Old rule | New rule |
|-----------|----------|----------|
| switch window | blocked if any agent busy | never blocked |
| new window | blocked if any agent busy | never blocked |
| close window | blocked if any agent busy | blocked only if *that* window busy |
| Enter on busy window | global `pending_prompt_` | enqueue on that window's slot |
| Enter on idle window while another runs | queued globally | starts its worker immediately |
| ESC / Ctrl+C / `/stop` | cancel everything | cancel the active window's run |
| Ctrl+C on idle window | quit | quit — confirm if `any_busy()`, then `cancel_all()` + `join_all()` |

**5. Per-window cancellation — `RunScope` thread-local.** A single
thread-local run scope in the core carries the calling agent's cancel
token, skill catalog, and window identity:

```cpp
// include/agent/run_scope.h — installed by the agent worker for the
// duration of Agent::run(); tools and dispatch resolve through it.
struct RunScope {
    const CancellationToken* cancel_token;   // falls back to host token
    SkillCatalog* skills;                    // falls back to bound catalog
    size_t window_id;
};
```

`WindowManager::new_window` gives each agent a fresh `CancellationToken`
(reset the copied flag before construction); `Agent::request_cancel()`
requests it. `Agent::run` installs the scope (RAII). Cancellable tools
consult `RunScope::cancel_token` before their registered host fallback —
`Tool::run`'s signature is unchanged and CLI/headless behavior is
identical (no scope installed → registered token).

**6. Skills and memories are shared per project — scope-resolved.**
Skills and memories are per-project data and amber is single-project
today, so all windows share them. The model, staged to match the roadmap:

- `WindowManager` owns a pool keyed by workspace root:
  `shared_ptr<SkillCatalog>` + `shared_ptr<MemoryStore>` per project.
  Today there is exactly one entry — every window's agent holds the same
  instances, so a skill authored or a memory learned in window A is
  immediately visible in window B (no per-window staleness, no refresh
  plumbing, and no torn `experience.json` writes from two store objects
  over one file).
- When per-window project switching lands, a window on another project
  resolves a different pool entry — per-project sharing falls out of the
  same mechanism. Amber-scope (global) skills are already a scan root
  (`SkillScanPaths::global`), so the future two-tier model needs no
  architectural change.
- Because the instances are shared, they get internal mutexes
  (`SkillCatalog`, `JsonMemoryStore`) and their ref-returning accessors
  (`entries()`, `overrides()`, `lookup`, `find_memory`, `find_skill`)
  return copies/optionals — an unlocked reference into shared mutable
  state would dangle under concurrent workers.
- **Activation stays per-agent.** `activated_` (session-activated skill
  bodies) moves off the catalog onto `Agent` — a skill read in window A
  must not inject into window B's prompts. `activate(name)` becomes
  `read_body` + the caller's sink.
- The skill tools resolve the *calling* agent's catalog through
  `RunScope::skills` rather than whichever catalog was bound last —
  correct by construction under both sharing models. The registered
  catalog remains the fallback for scope-less contexts (CLI, tests).

**7. Concurrency hygiene in existing code.** Remove `router_->clear()` from
`send_async` (would drop sibling events); `on_switch` stops clearing
`pending_tools_` wholesale; `drain_events` writes stats/ctx to the event's
window, not globals.

### Phase B — per-window settings semantics

- `/set model`, `/set policy.mode` (needs `Agent::set_mode` or
  `set_config(key,val)`), `/set reasoning effort`, think/detect toggles:
  apply to the **active window's agent**; `Tui::cfg_` remains the template
  for *new* windows. Broadcast loops over `window_manager_->all()` are
  deleted.
- Status bar + `/window list` show each window's model/mode so the
  independence is visible.

### Phase C — `/session fork`

- `completions.json`: `session.fork` node, `action: "core.session.fork"`,
  help/man per schema. Handler registered in `SlashDispatcher` as a pure
  `(action, arg)` closure.
- Reject while the source window's slot is busy (Context single-owner rule
  — no snapshot mid-mutation).
- `Agent::fork_from(const Agent& src)` in the core: copies `context_` (via
  `set_context`), `cfg_`, `meta_`, `session_approved_`, `model_windows_`,
  `turn_counter_`, `brief_store_`; fresh `ConversationLog`. Same class, so
  private access is clean; unit-testable in `run_tests.cpp`.
- Host side: `new_window(title)` → `fork->agent->fork_from(*src.agent)` →
  `SessionStore::new_id()` + `meta["forked_from"] = parent_id` → autosave
  both legs → save workspace.
- **KV-cache reuse by construction:** `ensure_system_prompt` seals the
  system prompt as `context[0]`, so copying the message deque preserves the
  wire prefix byte-for-byte; the fork's first request reuses the server's
  prefix cache for the entire shared history. Reuse holds while
  model/mode/thinking/tools stay identical — tail-injected prompt blocks
  (memory, briefs, plugin blocks) are appended to the request copy after
  the conversation and cannot dirty the shared prefix. Switching model on a
  fork forfeits reuse; that is user intent, not a defect.
- The fork's `{ts}` JSONL transcript starts at the fork point; full
  history lives in its session file.

## RED tests (written first, must fail)

### RunRegistry — 8 tests (`tests/tui_tests.cpp`)

1. `run_registry_slot_create_on_demand`
2. `run_registry_busy_is_per_window` — slot(1) busy does not make slot(2) busy
3. `run_registry_any_busy_aggregates`
4. `run_registry_cancel_is_per_window`
5. `run_registry_pending_enqueue_and_pop_fifo`
6. `run_registry_pending_isolated_per_window`
7. `run_registry_erase_idle_slot`
8. `run_registry_join_all_joins_workers`

### KeyBinder / WindowOps rescope — 6 tests (update + new)

9. `keybinder_busy_does_not_block_window_switch` — flips
   `keybinder_busy_state_blocks_window_switch` (behavior change: RED by
   inversion)
10. `keybinder_busy_does_not_block_new_window`
11. `windowops_switch_always_allowed` — mock port `is_busy` true → switch succeeds
12. `windowops_close_busy_window_rejected` — per-target busy check
13. `windowops_close_idle_window_while_sibling_busy` — succeeds
14. `inputstate_busy_reflects_active_window_only`

### Per-window state — 4 tests

15. `window_state_fields_are_per_window` — stats/ctx on `Window`, not `Tui`
16. `send_async_queues_on_target_window` — busy window gets pending, not global
17. `done_drains_window_pending_queue`
18. `snapshot_uses_window_stats` — session file carries its own window's telemetry

### Skill catalog decoupling — 1 test

19. `second_window_does_not_rebind_skill_tools` — window A `read_skill` still
    resolves A's catalog after window B is created

### Fork — 6 tests (`tests/run_tests.cpp` + `tests/tui_tests.cpp`)

20. `agent_fork_copies_identical_context` — `get_all()` equal, hash chain valid
21. `agent_fork_legs_diverge_independently` — push on fork does not touch parent
22. `agent_fork_copies_meta_approvals_and_model_windows`
23. `agent_fork_fresh_conversation_log`
24. `session_fork_action_creates_window_with_context` — TUI side
25. `session_fork_rejects_while_busy`

### Command tree — 1 test (`tests/completions_test.cpp`)

26. `session_fork_node_resolves_action` — `session.fork` → `core.session.fork`

**26 tests.** All pure except the thread-join lifecycle test, which uses a
trivial `std::thread([]{})` — no ncurses, no LLM.

## Scope / files

### Phase A (branch `fix/multi-window-concurrent-sessions`)

New:
- `tui/run_registry.h` / `tui/run_registry.cpp`

Modified:
- `tui/event_router.h/.cpp` — drop `thread_`/`busy_`/`cancel_`;
  `make_hooks(window_id, RunSlot&)`
- `tui/window.h` — per-window run-display fields
- `tui/window_ops.h/.cpp`, `tui/window_ops_port.h`,
  `tui/tui_window_ops_hooks.cpp` — `is_busy(idx)` per-target; switch never gated
- `tui/key_binder.cpp`, `tui/input_state.h` — busy = active window only;
  switch/new unconditional
- `tui/tui.cpp` — `send_async`/`compress_worker`/`agent_worker` run on slots;
  per-window pending drain; cancel/quit paths via RunRegistry
- `tui/tui.h` — remove migrated globals; add `RunRegistry`
- `tui/window_manager.cpp` — fresh cancel token per agent; skill-tool fix
- `tui/tui_session.cpp` — `snapshot` reads window fields
- `tui/render_engine.cpp` — status bar reads active window state
- `tui/tui_input.cpp` — `busy_reject` → active window; `/stop` → slot cancel
- `include/agent/agent.h`, `lib/agent.cpp` — `Agent::request_cancel()`
- `lib/`/`tools/` — TLS current-cancel-token seam (or documented deferral)
- `Makefile.in`, `tests/tui_tests.cpp`, `docs/fix-tracker.md`,
  `docs/issues.md`

### Phase B (same or follow-up branch)

- `include/agent/agent.h`, `lib/agent.cpp` — `Agent::set_mode` (or scoped
  config setter); expose current model/mode for the status bar
- `tui/tui_input.cpp` — settings setters target `win().agent`, delete
  broadcast loops
- `completions.json` — man text updated to per-window semantics

### Phase C (branch `feat/session-fork`)

- `include/agent/agent.h`, `lib/agent.cpp` — `Agent::fork_from`
- `completions.json` — `session.fork` node
- `tui/tui_input.cpp`, `tui/tui_session.cpp` — action closure + fork plumbing
- `tests/run_tests.cpp`, `tests/completions_test.cpp`
- `docs/spec/session/` — short spec note on fork + lineage + KV-reuse
  invariant

## Verification

- `make clean && make` (headers touched — regenerate `.d`)
- `make test` green: 26 new tests, existing suite unaffected except the
  intentionally inverted busy-gate tests
- `make lint` (clang-tidy) and `make analyze` (cppcheck) clean
- `CXX=g++` and `CXX=clang++` build/test matrix
- Manual: agent running in window 1; Alt+2 switches; window 2 runs a second
  agent concurrently; Esc cancels only the active window; `/window list`
  shows per-window state; closing a busy window is rejected, idle closes
  fine; quit with running agents confirms then joins all workers.
- Fork manual: `/session fork` on an idle window produces a second window
  with identical scrollback/context; both diverge independently; both
  autosave; session files carry `forked_from` lineage.

## Workflow

Red (26 failing tests) → Proposal (this doc) → **Sign-off** → Green → PR.
No production code is written until this proposal is approved.

## Implementation status (2026-09-13)

**Implemented.** All three phases landed:

- `tui/run_registry.{h,cpp}`: per-window `RunSlot` (thread, busy, cancel,
  FIFO pending queue), `RunRegistry` keyed by stable window id. `erase()`
  joins a finishing worker before destroying its slot; `request_cancel`
  creates the slot so a pre-dispatch cancel sticks.
- `include/agent/run_scope.h`: thread-local `RunScope` (cancel token,
  catalog, activation sink, parent chain), `ScopedRunScope` RAII,
  `run_cancelled`/`effective_catalog`/`record_activation` helpers.
- `Agent`: `fork_from`, `request_cancel`, `config()`/`set_mode`/
  `set_thinking` accessors; `run()`/`compress_now()` install the scope and
  clear a stale cancel flag. Per-window tokens come from the composition
  root (`WindowManager` gives each window's cfg a fresh
  `CancellationToken`; sub-agents get their own too), keeping the
  injected-token contract intact for tests/CLI.
- Shared project resources: `WindowManager` owns a workspace-keyed pool of
  `shared_ptr<SkillCatalog>` + `shared_ptr<MemoryStore>`; both classes are
  internally mutexed and return copies (`lookup`/`entries`/`overrides`,
  `find_memory`/`find_skill`). `skill_export`/`compressor_apply` were
  fixed to not hold pointers into snapshots.
- TUI: `Window` carries `state`/`stats`/`ctx_used`/`ctx_estimate`/
  `live_ctx_offset`/`running_tool`/`compressing`; `EventRouter` routes
  every event to its stamped window; switching/creating never gated;
  close rejected only for the busy target; Esc/Ctrl+C//stop cancel the
  active window; quit-with-running-agents confirms then cancels+joins all;
  approval dialogs label the asking window; idle Ctrl+C restores the
  intended quit path.
- Settings: `/set model`, `policy.mode`, `think`, `reasoning.effort`,
  compression and detection knobs, policy rules, provider switch all apply
  to the ACTIVE window's agent; `Tui::cfg_` stays the new-window template.
- `/session fork`: `session.fork` node in `completions.json` →
  `core.session.fork` → `SessionController::fork_session()` — new window,
  `Agent::fork_from` clones context/meta/approvals/model-windows/turn
  counter/briefs, fresh cancel token and client, `forked_from` lineage,
  both legs autosaved.

Verification: `make clean && make` green; `make test` 777/777 + all suites;
`make lint`/`make analyze` identical to the pre-existing baseline (30 and
27 findings respectively — none introduced by this change; the baseline
is tracked separately). Local `g++`/`clang++` both resolve to Apple clang
21; the real GCC leg runs in CI.

## Sign-off decisions (resolved 2026-09-13)

1. **Close-while-running:** REJECT with "cannot close window while its
   agent is running". Keeps the Window/agent/thread lifetime invariant
   trivial.
2. **Tool-level cancel:** thread-local `RunScope` (approved) — no
   `Tool::run` signature change; tools consult the scoped token before the
   host-bound fallback.
3. **Skills/memory:** SHARED PER PROJECT (corrected 2026-09-13 — amber is
   single-project today, so all windows share one catalog and one memory
   store; future per-window project switching resolves a different pool
   entry; amber-scope is already a scan root for the later two-tier
   model). `WindowManager` owns a project-keyed pool of
   `shared_ptr<SkillCatalog>` + `shared_ptr<MemoryStore>`; both get
   internal mutexes. Session activation state (`activated_skills`) moves
   onto `Agent` — it is per-window context, not project data. Skill tools
   resolve the *calling* agent's catalog through `RunScope` (correct under
   both today's shared and the future per-project model).
4. **Settings scope:** ACTIVE WINDOW — `/set model`, `/set policy.mode`,
   think, reasoning effort apply to `win().agent`; `Tui::cfg_` is the
   template for new windows. Also fixes the dead `/mode` setter.
