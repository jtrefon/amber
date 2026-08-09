# AGENTS.md — amber (cpp-agent)

C++17 AI agent harness: a core library (`libagent_core.a` + `libagent_tools.a`)
plus a headless CLI (`amber-cli`) and an ncurses TUI (`amber`, the flagship),
driven by an OpenAI-compatible LLM API.

## Build & verify

- `make` works from a fresh checkout: `GNUmakefile` auto-runs `./configure` to
  generate `Makefile` from `Makefile.in`. You rarely need to call `./configure`
  by hand.
- `make` builds everything (`lib cli tui`). The binaries and archive land in the
  repo root (`amber`, `amber-cli`, `libagent_core.a`, `libagent_tools.a`) — in-tree, not in a `build/`.
- `make test` builds and runs the unit suite (`run_tests`). `make check` is a
  separate, lighter gate (`smoketest` + `tests/build_hygiene.sh` build
  invariants) — do not confuse the two.
- `make lint` runs **clang-tidy** over every project source (third_party
  excluded) using the `.clang-tidy` config in the repo root. It is fast enough
  to gate changes on. `make analyze` runs **cppcheck** as an independent,
  cross-TU second opinion (slower; runs in parallel and skips the vendored
  nlohmann/json header). Both must come back clean before a commit.
- CI blocks on `make && make test` under **both** `g++` and `clang++`
  (`CXX=g++` / `CXX=clang++`).
- `make lint` (clang-tidy) and `make analyze` (cppcheck) gate CI as separate
  compiler-agnostic jobs (single run each, independent of the compiler matrix),
  before the build/test matrix.
- `make clean` removes in-tree `.o`/`.d`/binaries; `make distclean` also drops
  the generated `Makefile`.

## Compilation gotchas

- Header dependency files (`.d`, via `-MMD -MP`) are generated, not committed
  (`*.d` is gitignored). If you change a struct layout or any header, rebuild —
  stale `.o` from missing `.d` entries silently causes ABI/heap-corruption bugs
  at runtime (called out in the Makefile). When in doubt, `make clean && make`.
- `include/agent/version.h` is **generated** by `./configure` from
  `version.h.in`; do not hand-edit it, and don't commit a stale one.
- `compile_flags.txt` (for clangd/editors) is minimal; the real include paths
  (`-Iinclude -Isrc -Itools -I.`) and flags come from the Makefile/configure.

## Architecture boundaries

- `lib/` + `include/agent/` is the UI-free core: LLM client, tool registry,
  agent loop, prompt/markdown loader, built-in tools. Keep UI concerns out.
- `src/amber-cli` is the headless CLI; `tui/` is the ncurses client (`amber`).
  Both only *link* `libagent_core.a` + `libagent_tools.a` and communicate via
  `AgentHooks`. `tui/` must never be depended on by `lib/`.
- `bench/` is the benchmark & KPI harness (`amber-bench`): scenario loader,
  oracle scorer, recorder (an `AgentHooks` observer), KPI aggregation, static
  template engine. Same layering rules as the clients — `bench/` only links
  the libraries, never touches the engine. Hermetic mode (fake LLM) is
  deterministic and safe for CI; live mode targets any OpenAI-compatible
  endpoint. Specs: `docs/spec/benchmark/`.
- Tools live in `tools/` (read/write/search/bash). The search tool is
  pluggable: `mode="grep"` (default, wraps `grep -rnI`) or `mode="semantic"`
  (dependency-free lexical index). Swap only `embed()` to use a real model.
- System/tool prompts are Markdown in `prompts/` (`system.md`, `tools.md`),
  loaded at runtime — editing those changes agent behavior without recompiling.

## Conventions

- Style: `.clang-format` (LLVM-based, 4-space, no tabs, 100 cols). Run
  `clang-format -i <files>` on touched code. No comments that restate code.
- New source files need no copyright/SPDX header — keep the first line functional.
- Commits: imperative mood, scoped prefixes (e.g. `tui: fix drawer scroll`).
  Tests for behavior changes go in `tests/run_tests.cpp`.

## Security model (treat LLM output as untrusted)

- read/write confine paths to the workspace root (default cwd, override with
  `AMBER_WORKSPACE`); absolute paths and `../` escapes are rejected.
- bash tool is approval-gated and fail-safe (denied if no approver). CLI prompts
  on a TTY, denies when stdin is not a TTY unless `--yes`. TUI shows a dialog.
  Default timeout 60s, output capped 64 KiB.
- Keep the agent unprivileged (container / dedicated dir).

## Command tree architecture (JSON-driven, zero hardcoded completion)

`completions.json` is the **single source of truth** for slash-command
structure: namespace branches (unlimited nesting), short help (drawer),
man pages (`?` popup), and leaf `action`s (internal command mapping — what
the branch executes). Dispatch (`handle_slash`), completion
(`update_completions`), and the drawer (`draw_drawer`) all derive from the
tree in `SettingRegistry`; C++ handlers are pure `(action, arg)` closures.

- **Schema**: every node is `{help, man, action?, children{...}}`; the last
  leaf of a branch carries the `action` (e.g. `core.config.get.model.list`).
- **Namespaces are keyed by their full display path** (`get.model` ≠
  `set.model`); dotted `/get` lookups resolve exact → `get.<key>` →
  `set.<key>` (`resolve_key` in setting_registry.cpp).
- **Dynamic values are feed leaves**: runtime state and external
  integrations merge leaf subtrees via `merge_completions_json` (deep merge —
  static fields preserved, children unioned; MCP/plugin/feeds never clobber
  documented nodes). Each leaf carries a **generated action**
  (`<parent action>.<leaf key>`) and the feed registers the handler closure.
  Existing feeds: `refresh_model_list` (set.model), `refresh_policy_feed`
  (get/set policy rule — the permission system, unchanged, surfaced in the
  tree), `refresh_job_feed` (job kill/read), plus `mcp_completion_subtree`.
  New dynamic content = a new feed, never a C++ completion lambda.
- `SettingRegistry::complete(ns)` returns the **direct children** of a
  namespace (tree-walked) so drawer rows and completions stay 1:1 for Enter
  dispatch. The legacy flat `palette::Command` carries display metadata only
  (name/aliases/usage/help); there is no `complete_arg`/`current_value`.

### Hard rule: slash commands are NEVER hardcoded (no exceptions)

Every slash-command path, completion, and dispatch must come from the JSON
tree + feeds. Concretely:

- **No hardcoded command paths in handlers.** `handle_slash` walks the tree
  and dispatches the deepest documented node's `action`; C++ handlers are
  pure `(action, arg)` closures. Do NOT special-case command names in
  `cmd_set`/`cmd_get`/`handle_slash` — if a command is missing from the
  tree, add the node (or a feed leaf), not an `if (arg.rfind(...))`.
- **No hardcoded completion/choice lists.** Completion rows, `choices`,
  usage hints, and "try: ..." messages derive from the tree/feeds —
  including dynamically discovered names (providers, policy rules, models,
  MCP servers): they are feed leaves (`merge_completions_json`), never
  hardcoded C++ lists.
- **No dead legacy dispatch.** When a feed/tree supersedes a hand-written
  branch, delete the branch; a branch that "only sees the bare namespace"
  is acceptable only as the namespace's usage page (its `action` is
  registered).
- Feeds register their leaf action closures exactly like static nodes;
  `register_action` is the only place command behavior exists.

The command surface is `completions.json` + `refresh_*_feed()`s; anything a
user can type must be resolvable there.

## Context stack architecture (immutable, hash-chained)

The `Context` class (`include/agent/context.h`) is a **pure stack** — messages are
sealed on `push()` and can never be modified in-place. The only mutation
operations are:

| Operation | What it does |
|-----------|-------------|
| `push(msg)` | Append a sealed message to the **top** of the stack. |
| `pop()` | Remove the **most recently pushed** message (LIFO). Used by compression to push a classify/extract request, call the LLM, then pop it. |
| `clear()` | Remove all messages. Used by compression rebuild after assembly. |
| `get_all()` | **Read-only** view of the entire stack. Asserts FNV-1a hash-chain integrity before returning. |

Every `push()` computes `h_i = FNV(prev_hash || msg)` and stores it in a parallel
deque. `pop()` restores the previous hash in O(1). `get_all()` recomputes the
entire chain from the stored messages — any in-place mutation (`const_cast`,
rogue `replace` method, direct deque access) breaks a link and crashes with
`assert` in debug builds.

**Rules:**
- NEVER add a mutation method (replace, insert, update, set_message, etc.).
  If you need to rebuild the context, call `clear()` then `push()` each message.
- NEVER modify a message after it has been pushed (including via `const_cast`).
- The `assert(verify_chain())` in `get_all()` is the integrity gate. If you
  bypass `get_all()` to read the deque directly, you are responsible for
  verifying the chain yourself.
- See `tests/run_tests.cpp` (`context_hash_chain_integrity`) for the test that
  exercises every mutation path and verifies the chain survives.

## Runtime / config

- `amber.conf` sets `api_base`/`model`/`system_prompt`/`tools_prompt`. Defaults
  point at a local OpenAI-compatible endpoint (`localhost:8081/v1`).
- Streaming via SSE; disable with `--no-stream` or `AMBER_STREAM=0`.
- Releases are tag-driven (`vX.Y.Z`; tags with `-` are pre-releases) — see
  `.github/workflows/release.yml`.

## Engineering principles (mandatory)

These are hard requirements for every change. The bar is **zero technical debt**:
leave code in better shape than you found it (Boy Scout rule) — never commit a
known mess, even in adjacent code.

- **SOLID** must hold:
  - *SRP* — a class has one reason to change.
  - *OCP* — open for extension, closed for modification (add tools/backends via
    new types, not edits to the loop).
  - *LSP* — subtypes (every `Tool`/`SearchBackend`) must be substitutable.
  - *ISP* — narrow interfaces (`Tool`, `SearchBackend`, `AgentHooks`) only.
  - *DIP* — depend on abstractions (`Tool`, `SearchBackend`, `LLMClient`), not
    concretions; wiring happens at the boundary (CLI/TUI).
- **KISS / DRY / YAGNI** — no speculative generality, no duplicated logic. If you
  copy a block, extract it. If a feature isn't required now, don't add it.
- **Size limits** (enforced in review, not by the compiler):
  - A class/struct definition should stay **under 200 lines**. Split larger
    types (see Audit below).
  - A method/function should stay **under 10 lines** with **minimal branching**.
    Extract loops, parsing, and branching into named helpers.
- **Layering / isolation** — this repo uses a **hexagonal (ports & adapters)**
  style, not strict N-layer:
  - *Domain core* (`lib/` + `include/agent/`) defines the ports (`Tool`,
    `SearchBackend`, `LLMClient`, `AgentHooks`) and the agent use-case. No UI,
    no `main`, no linker dependency on `tui/` or `src/`.
  - *Adapters* live in `tools/` (tool adapters), `tools/search/` (search
    backends), and the clients `src/amber` (`main.cpp`) and `tui/` (ncurses).
    Adapters depend inward on the core; the core never depends outward.
  - Keep the dependency arrows pointing at the core. If `lib/` `#include`s
    anything from `tui/`, `src/`, or `tools/` (except the tool interface
    headers), that is an isolation violation.

## Development workflow

### Branching strategy

- **`main`** is the stable, release-ready branch. Always green. No direct pushes.
- Every fix or feature lives on a **feature branch** named `<type>/<short-description>`:
  - `fix/detached-thread-use-after-free`
  - `refactor/cancel-token-to-core`
  - `docs/add-tdd-policy`
- Branches are short-lived (days, not weeks). Open a **draft PR** early for
  visibility, mark it ready for review when all checks pass.
- Merge via **squash-merge** to keep `main` history clean. The squashed commit
  message must follow the imperative, scoped convention
  (e.g. `fix: cancel token now lives in core, not bash_tool globals`).

### Fix workflow — Red → Proposal → Sign-off → Green → PR

Every bug fix and every feature MUST follow this strict sequence:

```
┌──────────────────────────────────────────────────────────────┐
│  1. RED — Write a failing test that reproduces the bug or    │
│     specifies the desired behaviour. Commit it on the branch │
│     so CI shows the failure.                                 │
│                                                              │
│  2. PROPOSAL — Draft the architecture refactor in the PR     │
│     description or a linked doc (see docs/fix-tracker.md).   │
│     Describe target state, not the diff.                     │
│                                                              │
│  3. SIGN-OFF — Reviewer approves the architecture proposal   │
│     before any production code is written.                   │
│                                                              │
│  4. GREEN — Implement the fix. Make the test pass. Refactor  │
│     to meet all Engineering Principles above. Run local      │
│     linting and static analysis every few edits (don't       │
│     batch all issues to the end). Address every clang-tidy   │
│     and cppcheck finding — zero warnings is the threshold.   │
│     If your editor has LSP (clangd) integration, keep the    │
│     diagnostics panel clean as you type; LSP-reported errors │
│     (type mistakes, missing includes, const correctness)     │
│     must be resolved before the next compile.                │
│                                                              │
│  5. PR — Open/update the pull request. Run final clean
│     verification: make clean && make && make test &&         │
│     make lint && make analyze. All must pass with zero       │
│     warnings. The reviewer verifies the diff matches the     │
│     proposal and that no lint/analysis regression was        │
│     introduced.                                              │
└──────────────────────────────────────────────────────────────┘
```

- Do NOT write production code before the failing test (step 1) exists.
- Do NOT implement without an approved proposal (step 3).
- A fix that "can't be tested" is a sign the architecture needs refactoring,
  not an excuse to skip the test.
- Lint and analysis findings are **blockers**, not suggestions. A PR with any
  new clang-tidy or cppcheck warning is rejected regardless of correctness.
  See the `make lint` / `make analyze` targets in the Build & verify section.

### Code review checklist

Every PR reviewer MUST verify:

- [ ] SOLID conformance: no new SRP violations, dependency direction is correct.
- [ ] Hexagonal boundaries intact: `lib/` never `#include`s from `tui/`, `src/`,
      or `tools/` (except tool interface headers).
- [ ] Size limits: classes ≤200 lines, methods ≤10 lines with minimal branching.
- [ ] Test sequence: the PR includes a red (failing) commit followed by a green
      fix commit (or a clear explanation if not possible).
- [ ] All CI checks pass: `make`, `make test`, `make lint`, `make analyze`.
- [ ] Zero dead code: no commented-out code, no stubs, no speculative branches.
- [ ] No SPDX/copyright boilerplate — first line is functional (`#include`, `#ifndef`, etc.).
- [ ] No new clang-tidy or cppcheck warnings.
- [ ] **Context is a pure stack** — only `push()`, `pop()` (LIFO), `clear()`, `get_all()`. No mutation of sealed messages. No `replace()` or similar. The FNV-1a hash chain in `get_all()` asserts integrity — any bypass crashes in debug.

## Prompting philosophy (mandatory)

Prompts are **descriptive, not prohibitive**: describe the role, personality,
environment and tooling, and empower the agent to work — never force or
forbid behavior ("never", "don't", "must", "do not" are banned from
`prompts/`). Conventions (like the closing `done` marker) are described as
the natural shape of finished work, not commands. A prompt change is a
behavior change: prove it with a before/after benchmark run (see
`BENCHMARK.md` "Prompt v2" section for the template).

## Coding standards

- **RAII** — ownership follows resource acquisition. Use `unique_ptr` for
  exclusive ownership, scoped objects on the stack, and `shared_ptr` only when
  ownership is genuinely shared. Never use raw `new`/`delete`.
- **Rule of Five / Zero** — prefer Rule of Zero (implicit special members are
  correct). When a destructor, copy constructor, copy assignment, move constructor,
  or move assignment is user-defined, explicitly declare all five or `= delete`.
- **`noexcept`** — mark pure accessors, trivial getters, and functions that
  never throw as `noexcept`. Only omit `noexcept` when the function legitimately
  throws. Every `Tool::name()`, `is_read_only()`, `requires_approval()`,
  `SearchBackend::name()`, `Config::api_url()` should be `noexcept`.
- **Const-correctness** — mark member functions and parameters `const` wherever
  possible. Use `const&` for read-only parameters of non-trivial types.

## Error handling conventions

- **Tools** — always return errors via `ToolResult{false, "", error_msg}`.
  Never throw from `Tool::execute()`. Catch unexpected exceptions and convert
  to `ToolResult`.
- **Library functions** — may throw `std::runtime_error` for truly exceptional
  conditions (transport failure, corrupt config). Do not throw for expected
  states (empty results, missing files) — return an error code, empty optional,
  or `ToolResult`.
- **Recoverable errors** — model errors (malformed JSON, HTTP 4xx/5xx) should
  be returned as assistant messages or error-flagged `ToolResult` so the LLM
  can self-recover.
- **Unrecoverable errors** — configuration corruption, libcurl init failure.
  Throw at construction; the host (CLI/TUI) catches and reports.
- **Assertions** — use `assert()` only for invariants that should never fire
  in a correct program. Never use asserts for input validation.

## TDD / Red-Green-Refactor (mandatory)

- **Bug fixes** must start with a failing test that reproduces the bug. Only
  then is the production code changed (Red → Green). After the fix passes,
  the test is committed alongside the fix.
- **New features** must follow the same cycle: write a failing test that
  specifies the desired behaviour, implement until green, then refactor.
- **Coverage threshold**: new code paths must have ≥80% line coverage. The CI
  gate (`make test`) must pass before merge.
- **Hermetic tests**: mock the LLM by testing `LLMClient::parse_models` /
  `merge_server_info` directly; do not hit a live server in the unit suite.
- **Test granularity**: prefer many small `TEST(name)` blocks over a single
  large test function. Each test exercises one behaviour.
- **Test location**: behaviour changes go in `tests/run_tests.cpp`. New test
  files may be added for major modules (`tests/compressor_test.cpp`,
  `tests/agent_test.cpp`) — add them to `UNITTEST_OBJ` in `Makefile`.

## Design patterns in use

- **Strategy** — `SearchBackend` (`grep` vs `semantic`), selected at runtime by
  the `search` tool's `mode` arg without changing the schema.
- **Factory** — `make_*_tool()` / `make_*_backend()` free functions return
  `unique_ptr<>` so the registry owns distinct instances; `register_default_tools`
  wires the standard set for every host.
- **Registry / Service Locator** — `ToolRegistry` owns and looks up tools by
  name for the agent loop and the LLM `tools[]` schema.
- **Observer** — `AgentHooks` (via `std::function` callbacks) lets UIs observe
  the agent loop without the core knowing about them. More precise than "Template
  Method" since the hooks are set, not subclassed.
- **Command** — `ProcessStartTool` / `ProcessReadTool` / `ProcessStopTool` each
  encapsulate a background-process request as an object with a uniform `execute()`.
- **Protection Proxy** — `Workspace::confine()` guards filesystem access behind
  path-confinement checks, proxying the real filesystem.
- **Null Object** — `Agent::silent_hooks()` returns a no-op `AgentHooks` so
  internal confirmation exchanges never reach the scrollback, without null-checking
  at every call site.
- **Memento** — `run_compression()` (gate path) and `compress_now()` leave the
  live `context_` untouched when the pipeline fails (spec invariant 7); the
  rebuild via `clear()` + `push()` only happens on success, capturing and
  rolling back state atomically.
- **Adapter** — `LLMClient` adapts libcurl + the OpenAI JSON contract behind a
  small C++ interface; `Workspace` adapts filesystem confinement behind a simple
  `confine()` port.
- **Facade** — `Agent` orchestrates client + registry + hooks + log into one
  `run()` use-case.

## Architecture audit (status: NON-CONFORMING on size limits)

Last reviewed against the limits above. The architecture and SOLID posture are
**sound** (clean hexagonal boundaries, correct abstraction, no core↔UI coupling),
but several files exceed the hard size limits and must be split before we can
claim 0-debt conformance. Line counts below are enforced by
`tests/build_hygiene.sh` (`make check`) — if they drift, refresh the table:

| File | Lines | Issue |
|------|------:|-------|
| `tests/run_tests.cpp` | 4161 | Test file; exempt from class-size rule but a candidate for per-area headers. |
| `lib/session.cpp` | 269 | OK, but `list()` mixes POSIX `opendir` with JSON — consider an `fs` helper. |
| `tui/tui_render.cpp` | 655 | Method implementations (not a class); exempt from class-size rule. |
| `tui/tui_input.cpp` | 2275 | Method implementations (not a class); exempt from class-size rule. |

### Resolved
- `lib/llm.cpp` (511 → 84): split into `request_builder`, `sse_parser`,
  `http_transport`, `model_probe`, `debug_log` (+ `llm.cpp` keeps the class).
- `lib/agent.cpp` (473 → 200, now 773): `run` decomposed into `confirm_turn`,
  `dispatch_tool_calls`, `agent_helpers`, `tool_recovery`; `compress_now` now
  delegates to `CompressionPipeline::compress()` via `compression_->compress()`.
- `tui/tui.cpp` (1245 → 171, now 1044): god-class `Tui` split into `tui.h` (declaration,
  +420 lines) + `tui_render.cpp`, `tui_input.cpp`, `tui_session.cpp`,
  `tui_main.cpp`. No file defines a class >200 lines.
- `tui/widgets.cpp` (333): split into `dialog.cpp`, `form_edit.cpp`,
  `info_dialog.cpp`, `menu_select.cpp`.
- `tools/search/semantic_backend.cpp` (227 → 126): free helpers extracted to
  `semantic_index.cpp` (107 lines) + `semantic_helpers.h`.
- `tools/bash_tool.cpp` (191 → 196): `execute()` decomposed into free helpers
  `run_with_timeout` + `drain_output` in the anonymous namespace.
- **Detached thread in `chat_once`** (Critical): replaced with synchronous
  extraction — `chat_once` no longer spawns a thread.
- **HTTP transport + tool-cancel globals** (Critical): `CancellationToken` in
  `include/agent/process.h`, used by `http_transport`; no module-level globals.
- **`Agent::run()` SRP** (High): decomposed into 4 named methods.
- **`Agent::compress_now()` SRP** (High): reuses `CompressionPipeline::compress()`
  via `compression_->compress()` with `CompressionObserver`.
- **Tool cancel globals** (High): instance-scoped `CancellationToken`.
- **Tools in `libagent.a`** (High): split into `libagent_core.a` +
  `libagent_tools.a` in `Makefile`.
- **Tests include TUI headers** (Medium): TUI tests moved to
  `tests/tui_tests.cpp`; `tests/run_tests.cpp` is TUI-header-free.

All items previously listed in "Current outstanding issues" have been resolved.
See `docs/issues.md` for the historical register and `docs/fix-tracker.md` for
fix details.

When refactoring to fix these, preserve behavior and keep `make test` green. Run
`make clean && make` after touching headers (see Compilation gotchas).
