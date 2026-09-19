# RunScope does not survive the tool-dispatch thread hop

Issue: tool cancellation, skill-catalog resolution, and skill-activation
bookkeeping are silently inert whenever a tool runs through `dispatch_tool_calls`
— i.e. for every tool the model calls. (New `docs/issues.md` entry, `FIX-033`,
to be registered with this proposal.)

Follow-up defect in the work signed off in
`docs/fix-proposal/multi-window-concurrent-sessions-2026-09-13.md`, sign-off
decision **"Tool-level cancel: thread-local `RunScope` (approved) — no
`Tool::run` signature change"**.

## Problem

### 1. The symptom

`RunScope` is a `thread_local` ambient context installed by `Agent::run` so that
shared tools resolve the **calling agent's** cancel token, skill catalog, and
activation sink rather than whatever was bound at registration
(`include/agent/run_scope.h:12-38`, installed at `lib/agent.cpp:920-924`).

But every approved tool is executed on a fresh `std::async` worker, not on the
agent thread:

| Site | File | Effect |
|------|------|--------|
| Tool execution on a new thread | `lib/dispatch.cpp:289-299` | worker's `t_run_scope` is `nullptr` |
| Only the sub-agent flag is propagated | `lib/dispatch.cpp:290` (`set_subagent_inherited`) | the run scope is not |
| Bash cancel polls the scope | `tools/bash_tool.cpp:255` | falls back to the registration token |
| Skill catalog resolved through the scope | `tools/skill_tools.cpp:103,168,233` | falls back to the bound catalog |
| Activation recorded through the scope | `tools/skill_tools.cpp:117` | no-op on the worker |

On a dispatch worker `current_run_scope()` is **always** `nullptr`, so all three
consumers take their registration-bound fallback path. Consequences:

- **In-flight shell commands cannot be cancelled.** `run_cancelled` falls back
  to the token bound at registration (`tools/bash_tool.cpp:310-311`). In the TUI
  that token is the shared template (`tui/tui_main.cpp:196`,
  `plugins/tool_bash/tool_bash_plugin.cpp:16-19`), while each window's agent owns
  a *fresh* token (`tui/window_manager.cpp:38-39`) that `Agent::request_cancel()`
  requests (`include/agent/agent.h:210`). The window's token never reaches the
  running command: Esc aborts the loop between iterations and the next LLM call,
  but a destructive `bash` call keeps running to its timeout.
- **Sub-agents cannot be cancelled by their parent.** `SubAgentExecutor` gives a
  sub-agent its own token and relies on the ancestor chain for parent cancel
  (`lib/subagent.cpp:85-88`: "Parent cancel still reaches a sub through the
  RunScope ancestor chain"). The sub-agent's `Agent::run` runs on the same
  scope-less worker, so its scope's `parent` is `nullptr` — cancelling the window
  does not stop its sub-agent.
- **Skill activation is lost.** `record_activation` (`tools/skill_tools.cpp:117`)
  is a no-op on the worker: the body is returned to the model but never
  re-injected into later turns' prompt copy (`lib/agent.cpp:857`), which is the
  documented per-session behaviour (`include/agent/agent.h:228-229`).

The code and docs assert the opposite of the runtime behaviour:

- `include/agent/run_scope.h:24-26` — "a sub-agent run inside a tool installs its
  own scope but remains cancellable through its ancestors' tokens".
- `include/agent/agent.h:207-208` — "cancellable tools consult it through
  RunScope".
- `tui/tui.cpp:301-304` — "the agent's own token aborts its run loop **and
  in-flight cancellable tools (RunScope)**".

### 2. Why the tests did not catch it

The run-scope tests call `record_activation` / `run_cancelled` /
`effective_catalog` **directly on the test thread** (`tests/run_tests.cpp:6317-6393`),
so a scope is always installed. The dispatch tests never install a scope at all
and only exercise approval/result plumbing (`tests/run_tests.cpp:3334-3486`). No
test drives a tool *through* `dispatch_tool_calls` while a scope is installed —
the exact combination that fails.

### 3. The design premise that broke

`RunScope` is a **mutable linked list**: `ScopedRunScope` sets `scope->parent =
previous` at install time (`include/agent/run_scope.h:48-52`). That is correct
only under the unstated assumption that the scope is installed and consumed on
**one thread**. The dispatch `std::async` hop violates it: the worker never
installs a scope, so it sees nothing.

The mutable `parent` write is also why the scope cannot simply be reused across
threads: installing the same object on two workers would race on `parent` and
corrupt the chain.

## Root cause

The run scope is a **thread-local ambient value installed only on threads that
call `Agent::run` / `run_compression`**. Tools execute on threads created *below*
that install point, and the scope is neither propagated across the thread
boundary nor representable as a transferable value (its chain is a shared,
mutated linked list). The fix is to make the scope a **self-contained value that
can be captured on one thread and installed on another**, and to propagate it at
the dispatch boundary.

## Target architecture

### 1. `RunScopeChain` — a flattened, transferable scope value

`include/agent/run_scope.h` is rewritten so the active scope is a **value** whose
ancestor chain is flattened at capture time, removing the shared mutable
`parent` link entirely.

```cpp
// The resources a tool resolves from the RUNNING agent (not from whatever
// object was bound at registration). One frame per run.
struct RunScope {
    const CancellationToken* cancel_token = nullptr;
    SkillCatalog* skills = nullptr;
    ActivationSink* activated = nullptr; // see companion change (2)
};

// The active scope for a thread, flattened into a value: the leaf frame plus
// the resolved ancestor cancel tokens (leaf-first). A value type, so it can be
// captured on one thread and installed on another — the property the dispatch
// path needs. Flattening at capture removes the shared, mutable parent link:
// installing a chain never touches another thread's objects.
struct RunScopeChain {
    RunScope leaf;
    std::vector<const CancellationToken*> cancel_chain; // leaf-first

    bool empty() const noexcept {
        return cancel_chain.empty() && leaf.skills == nullptr && leaf.activated == nullptr;
    }
};

inline thread_local const RunScopeChain* t_run_scope = nullptr;

inline const RunScopeChain* current_run_scope() noexcept { return t_run_scope; }

// Snapshot the calling thread's chain. Empty when no scope is installed.
inline RunScopeChain capture_run_scope() {
    return t_run_scope ? *t_run_scope : RunScopeChain{};
}

class ScopedRunScope {
public:
    // Build a chain from this thread's current scope and install it.
    explicit ScopedRunScope(const RunScope& leaf) : prev_(t_run_scope), installed_(&owned_) {
        owned_.leaf = leaf;
        if (prev_)
            owned_.cancel_chain = prev_->cancel_chain;
        if (leaf.cancel_token)
            owned_.cancel_chain.insert(owned_.cancel_chain.begin(), leaf.cancel_token);
        t_run_scope = installed_;
    }
    // Install a chain captured on another thread; the chain must outlive the
    // guard (bind it to a named local / lambda capture, never a temporary).
    explicit ScopedRunScope(const RunScopeChain& chain) : prev_(t_run_scope), installed_(&chain) {
        t_run_scope = chain.empty() ? prev_ : installed_;
    }
    ScopedRunScope(RunScopeChain&&) = delete; // no temporaries: address would dangle
    ~ScopedRunScope() { t_run_scope = prev_; }

    ScopedRunScope(const ScopedRunScope&) = delete;
    ScopedRunScope& operator=(const ScopedRunScope&) = delete;

private:
    const RunScopeChain* prev_;
    RunScopeChain owned_;
    const RunScopeChain* installed_;
};
```

Consumers become pure reads of a value (each ≤10 lines, no branching beyond one
loop):

```cpp
inline bool run_cancelled(const CancellationToken& fallback) noexcept {
    if (const RunScopeChain* s = t_run_scope) {
        for (const CancellationToken* t : s->cancel_chain)
            if (t && t->is_requested())
                return true;
    }
    return fallback.is_requested();
}

inline SkillCatalog& effective_catalog(SkillCatalog& fallback) noexcept {
    const RunScopeChain* s = t_run_scope;
    return (s && s->leaf.skills) ? *s->leaf.skills : fallback;
}

inline void record_activation(const std::string& name, const std::string& body) {
    const RunScopeChain* s = t_run_scope;
    if (s && s->leaf.activated)
        s->leaf.activated->record(name, body);
}
```

`Agent::run` / `run_compression` change one line each — a value instead of a
pointer (`lib/agent.cpp:920-924`, `439-447`):

```cpp
RunScope scope;
scope.cancel_token = &cfg_.cancel_token;
scope.skills = skills_.get();
scope.activated = &activated_skills_;
ScopedRunScope scope_guard(scope);
```

### 2. `ActivationSink` — the sink must become thread-safe (mandatory)

The fix makes `record_activation` reachable from **concurrent** workers for the
first time (two `read_skill` calls in one assistant message run in parallel). The
sink is a raw `std::vector<ActivatedSkill>` today (`include/agent/agent.h:392`)
with no lock, so propagating the scope without addressing this would *introduce*
a data race — a regression on the very class of bug being fixed.

Introduce a small owner that encapsulates the vector, the dedup rule, and its
own mutex (SRP; the dedup logic currently lives inline in `record_activation`):

```cpp
// include/agent/activation_sink.h
// Session-activated skill bodies for ONE agent. Reachable from tool workers
// (read_skill runs on a dispatch thread), so it is internally synchronized.
class ActivationSink {
public:
    void record(const std::string& name, const std::string& body); // lock + dedup + push
    std::vector<ActivatedSkill> snapshot() const;                   // lock + copy
    void assign(const std::vector<ActivatedSkill>& items);          // lock + replace (fork)
private:
    mutable std::mutex mtx_;
    std::vector<ActivatedSkill> items_;
};
```

`Agent` holds an `ActivationSink activated_skills_` instead of the vector. The
three touch points are contained:

- `include/agent/agent.h:392` — field type; `:229` accessor returns
  `snapshot()` by value (an unlocked reference into shared mutable state would be
  unsafe). The accessor has no callers today.
- `lib/agent.cpp:857` — `for (const auto& act : activated_skills_.snapshot())`.
- `lib/agent.cpp:286` — `fork_from` uses `assign(src.activated_skills_.snapshot())`.

`SkillCatalog` is already internally mutexed with copy-returning accessors
(`include/agent/skill_catalog.h:45-48,106`), so concurrent catalog resolution
through the scope needs no further change.

### 3. Propagate at the dispatch boundary

`lib/dispatch.cpp` captures the caller's chain once and installs it on every
worker (SRP: one capture, one guard):

```cpp
const RunScopeChain active = capture_run_scope(); // caller = the agent thread
...
pending.push_back({i, std::async(std::launch::async,
                                 [&todo, i, caller_in_subagent, active]() {
                                     ScopedRunScope scope_guard(active);
                                     set_subagent_inherited(caller_in_subagent);
                                     try {
                                         return todo[i].tool->execute(todo[i].args);
                                     } catch (const std::exception& e) {
                                         ToolResult r;
                                         r.error = std::string("tool threw: ") + e.what();
                                         return r;
                                     }
                                 })});
```

Why this is correct and safe:

- **Nested chains survive.** A sub-agent's `Agent::run` on the worker builds its
  chain from `prev_` (the worker's installed chain), so the window token is an
  ancestor of the sub-agent's token — parent cancel reaches the sub-agent.
- **No shared mutation.** Each lambda owns its captured `active` (per-worker
  copy); installing it only reads. Two workers never touch the same mutable
  object.
- **Lifetime is bounded by the Agent, not a stack frame.** The chain holds raw
  pointers to `Agent` members (`cfg_.cancel_token`, `skills_`,
  `activated_skills_`) that outlive the run. `dispatch_tool_calls` additionally
  joins every future before returning (`lib/dispatch.cpp:352-364`), so the chain
  also outlives the workers even if a future call site detaches.
- **No-scope hosts are unchanged.** With no scope installed,
  `capture_run_scope()` is empty and `ScopedRunScope` leaves `t_run_scope` at
  `nullptr`, so CLI/headless/tests keep the exact fallback behaviour documented
  at `include/agent/run_scope.h:28-30`.

Both dispatch call sites are inside `Agent::run` (`lib/agent.cpp:577,643`) and
thus under the installed scope; both are fixed by the same change.

## Alternatives considered and rejected

1. **Immutable scope, re-parent at construction, share the pointer across
   workers.** Smallest diff (drop the `parent` write from `ScopedRunScope`, set
   `parent` at construction). Rejected: it relies on the fragile invariant that
   every worker joins before the scope dies, and leaves a mutable linked list
   read concurrently by many threads — a rule the type does not enforce. The
   value-chain makes the safety a property of the data.
2. **Explicit context parameter on `Tool::execute` (no ambient TLS).** Rejected:
   it changes the narrow `Tool` port (`include/agent/tool.h`) and every tool
   implementation, directly contradicting the approved sign-off decision
   ("no `Tool::run` signature change") and widening the interface (ISP).
3. **Rebind tools per window at registration.** Rejected: reintroduces exactly
   the cross-talk the prior proposal fixed (the newest window's catalog hijacking
   the binding); a shared registry is intentional.
4. **Run tools synchronously on the agent thread.** Rejected: removes parallel
   tool execution, a deliberate feature, and changes approval/timing semantics.

## Scope / files (branch `fix/run-scope-cross-thread`)

New:

- `include/agent/activation_sink.h` (header-only; no `Makefile.in` change)

Modified:

- `include/agent/run_scope.h` — value chain, capture/install, consumers
- `lib/dispatch.cpp` — capture + per-worker install
- `include/agent/agent.h` — `activated_skills_` type; accessor returns snapshot
- `lib/agent.cpp` — scope construction (x2), `inject_prompt_blocks`, `fork_from`
- `tests/run_tests.cpp` — new dispatch-scope tests; update the 3 existing
  scope-chain tests to the value API (`tests/run_tests.cpp:6324-6393`)
- `tests/agent_loop_test.cpp` — end-to-end `Agent::run` cancellation test
- `docs/issues.md`, `docs/fix-tracker.md` — register `FIX-033`

No change: `include/agent/tool.h`, any `Tool` implementation signature,
`skills_catalog.h`.

## RED tests (written first, must fail)

### Dispatch-level — `tests/run_tests.cpp` (the tightest reproduction)

1. `dispatch_worker_sees_installed_run_scope` — install a leaf scope; a probe
   tool records `current_run_scope()`. Assert the worker saw a non-null scope and
   the same `cancel_token`. **Fails today** (nullptr on the worker).
2. `dispatch_worker_cancel_reads_scoped_token` — scope token requested, fallback
   token never requested; probe returns `run_cancelled(fallback)`. Assert true.
   **Fails today** (worker only sees the fallback).
3. `dispatch_worker_resolves_scoped_catalog` — scope points at catalog B; probe
   returns `&effective_catalog(catalog_a)`. Assert `== &b`. **Fails today.**
4. `dispatch_worker_records_activation` — scope carries a sink; probe calls
   `record_activation("x","body")`. Assert the sink has one entry. **Fails today**
   (no-op).
5. `dispatch_nested_scope_keeps_ancestor_token` — probe installs its own leaf
   scope (simulating a sub-agent) and returns `run_cancelled(fallback)`; assert
   the *outer* requested token is seen through the chain. **Fails today.**
6. `dispatch_sibling_scopes_do_not_cross_cancel` — two threads, each installing
   its own scope, each running dispatch; request token A only. Assert A's probe
   sees cancellation and B's does not. The probe must pass a **genuinely inert
   fallback** to `run_cancelled`: the check is `chain || fallback`, so reusing
   the other sibling's token as the fallback makes the assertion vacuous.
   **Fails today** (and guards the fix).
7. `activation_sink_thread_safe` — N threads `record` distinct names; assert the
   snapshot has N entries. Meaningful under the ASan job and a local TSan run.

### End-to-end — `tests/agent_loop_test.cpp`

8. `agent_loop_tool_cancel_reaches_in_flight_tool` — `FakeLLMClient` scripts one
   tool call; a blocking probe polls `run_cancelled(never)`; the test thread calls
   `ag.request_cancel()`. Assert the probe unblocked and the run finished
   cancelled. **Fails today** (probe never observes the token).

Existing tests `run_scope_run_cancelled_walks_parent_chain` and
`run_scope_nested_restores_outer` (`tests/run_tests.cpp:6335-6365`) are updated to
the value API (chain built by nested `ScopedRunScope`); assertion intent is
unchanged.

## Verification

- `make clean && make` (headers touched — regenerate `.d`, per AGENTS.md)
- `make test` green: 8 new tests; existing suite unaffected except the two
  chain-shape updates
- `make lint` (clang-tidy) and `make analyze` (cppcheck) clean
- `CXX=g++` and `CXX=clang++` matrix
- ASan+UBSan job covers the sink test; **additionally run a local TSan build**
  (`-fsanitize=thread`) over `run_tests` to validate the sink and the per-worker
  install, since CI has no TSan job (proposing one is a separate follow-up)
- Manual: window runs `bash` with a long command; Esc stops it promptly. Two
  windows run concurrently; cancelling one does not disturb the other. `read_skill`
  followed by a second turn re-injects the body. A `task` sub-agent is cancelled
  when its window's Esc is pressed.

## Design validation (prototype)

The `run_scope.h` / `activation_sink.h` design above was prototyped standalone and
compiled and run before this proposal was written, to confirm the value-chain
semantics and the cross-thread install hold in practice:

- `clang++ -std=c++17 -Wall -Wextra -pthread` — clean, no warnings.
- ASan + UBSan (`-fsanitize=address,undefined`) — all assertions pass.
- TSan (`-fsanitize=thread`) — no data races, including 64 concurrent
  `record_activation` calls into one `ActivationSink` through per-worker installed
  chains.

Verified behaviours: no-scope workers still see `nullptr` (fallback path
preserved); an installed scope reaches the worker with its token; two sibling
threads' scopes do not cross-cancel; a nested inner scope sees the outer
(ancestor) token; the sink loses no concurrent records.

## Performance and risk

- **Cost:** one small `std::vector` copy per `capture_run_scope()` (once per
  dispatch) plus one per worker; `run_cancelled` iterates a short vector instead
  of a linked list (same order). Prompt rendering copies the activation vector
  once per turn. Tool execution is not a per-token path; the cost is negligible.
- **Blast radius:** contained to `run_scope.h`, one dispatch function, and the
  `Agent` activation field. No `Tool` interface, no provider/dialect, no plugin
  contract, no command tree.
- **Behavioural risk:** none for hosts without a scope (fallback path preserved);
  the TUI gains the cancellation and skill behaviour its comments already claim.

## Related defects surfaced (out of scope — file separately)

These share the area but are not required to fix `FIX-033`; recording them so
they are not lost:

- **The TUI template token is never requested.** `cfg.cancel_token` is bound to
  MCP servers (`tui/tui.cpp:69`) and to the bash fallback
  (`tui/tui_main.cpp:196`) but no code ever calls `.request()` on it; only
  per-window tokens are requested (`tui/tui.cpp:301-311`). MCP request
  cancellation is therefore inert, and that dead fallback is what made this bug
  silent. Decide the host-token contract (e.g. request it on global shutdown).
- `lib/agent.cpp:442` — `run_compression` clears the cancel token
  unconditionally; a concurrent cancel on the same agent could be lost.
- `lib/agent.cpp:915` and `:919` — redundant double `cfg_.cancel_token.clear()`.
- `PromptRegistry` is read on the agent thread and mutated on the UI thread with
  no lock (`include/agent/extensions.h:71-95`) — separate concurrency issue.

## Workflow

Red (8 failing tests) → Proposal (this doc) → **Sign-off** → Green → PR.
No production code is written until this proposal is approved.

## Sign-off decisions requested

1. **Design:** flattened value chain (`RunScopeChain`) with per-worker install —
   recommended — versus the immutable shared-pointer variant. (Recommendation:
   value chain; safety is enforced by the type, not by a lifetime rule.)
2. **Companion change:** include the `ActivationSink` mutex in this fix
   (recommended, mandatory) — without it the fix introduces a data race.
3. **Scope boundary:** keep this to core propagation + the sink lock, deferring
   the host-token/MCP wiring to a separate issue (recommended), or fold the
   template-token wiring into this change.
4. **Test surface:** add tests to both `tests/run_tests.cpp` (dispatch-level) and
   `tests/agent_loop_test.cpp` (end-to-end) — recommended — or dispatch-level
   only.
