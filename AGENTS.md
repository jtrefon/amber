# AGENTS.md — amber (cpp-agent)

C++17 AI agent harness: a core library (`libagent.a`) plus a headless CLI
(`amber`) and an ncurses TUI (`amber-tui`), driven by an OpenAI-compatible LLM API.

## Build & verify

- `make` works from a fresh checkout: `GNUmakefile` auto-runs `./configure` to
  generate `Makefile` from `Makefile.in`. You rarely need to call `./configure`
  by hand.
- `make` builds everything (`lib cli tui`). The binaries and archive land in the
  repo root (`amber`, `amber-tui`, `libagent.a`) — in-tree, not in a `build/`.
- `make test` builds and runs the unit suite (`run_tests`). `make check` is a
  separate, lighter tool smoke test (`smoketest`) — do not confuse the two.
- `make lint` runs **clang-tidy** over every project source (third_party
  excluded) using the `.clang-tidy` config in the repo root. It is fast enough
  to gate changes on. `make analyze` runs **cppcheck** as an independent,
  cross-TU second opinion (slower; runs in parallel and skips the vendored
  nlohmann/json header). Both must come back clean before a commit.
- CI blocks on `make && make test` under **both** `g++` and `clang++`
  (`CXX=g++` / `CXX=clang++`).
- `make clean` removes in-tree `.o`/`.d`/binaries; `make distclean` also drops
  the generated `Makefile`.

## Compilation gotchas

- Header dependency files (`.d`, via `-MMD -MP`) are committed. If you change a
  struct layout or any header, rebuild — stale `.o` from missing `.d` entries
  silently causes ABI/heap-corruption bugs at runtime (called out in the
  Makefile). When in doubt, `make clean && make`.
- `include/agent/version.h` is **generated** by `./configure` from
  `version.h.in`; do not hand-edit it, and don't commit a stale one.
- `compile_flags.txt` (for clangd/editors) is minimal; the real include paths
  (`-Iinclude -Isrc -Itools -I.`) and flags come from the Makefile/configure.

## Architecture boundaries

- `lib/` + `include/agent/` is the UI-free core: LLM client, tool registry,
  agent loop, prompt/markdown loader, built-in tools. Keep UI concerns out.
- `src/amber` is the headless CLI; `tui/` is the ncurses client. Both only
  *link* `libagent.a` and communicate via `AgentHooks`. `tui/` must never be
  depended on by `lib/`.
- Tools live in `tools/` (read/write/search/bash). The search tool is
  pluggable: `mode="grep"` (default, wraps `grep -rnI`) or `mode="semantic"`
  (dependency-free lexical index). Swap only `embed()` to use a real model.
- System/tool prompts are Markdown in `prompts/` (`system.md`, `tools.md`),
  loaded at runtime — editing those changes agent behavior without recompiling.

## Conventions

- Style: `.clang-format` (LLVM-based, 4-space, no tabs, 100 cols). Run
  `clang-format -i <files>` on touched code. No comments that restate code.
- Every new source file needs the SPDX/Apache-2.0 header:
  `// SPDX-License-Identifier: Apache-2.0` + `// Copyright 2026 Jacek Trefon`.
- Commits: imperative mood, scoped prefixes (e.g. `tui: fix drawer scroll`).
  Tests for behavior changes go in `tests/run_tests.cpp`.

## Security model (treat LLM output as untrusted)

- read/write confine paths to the workspace root (default cwd, override with
  `AMBER_WORKSPACE`); absolute paths and `../` escapes are rejected.
- bash tool is approval-gated and fail-safe (denied if no approver). CLI prompts
  on a TTY, denies when stdin is not a TTY unless `--yes`. TUI shows a dialog.
  Default timeout 60s, output capped 64 KiB.
- Keep the agent unprivileged (container / dedicated dir).

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
- [ ] SPDX/Apache-2.0 header on every new file.
- [ ] No new clang-tidy or cppcheck warnings.

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
- **Memento** — `compress_now()` snapshots `history_` before mutation and
  restores it on failure, capturing and rolling back state.
- **Adapter** — `LLMClient` adapts libcurl + the OpenAI JSON contract behind a
  small C++ interface; `Workspace` adapts filesystem confinement behind a simple
  `confine()` port.
- **Facade** — `Agent` orchestrates client + registry + hooks + log into one
  `run()` use-case.

## Architecture audit (status: NON-CONFORMING on size limits)

Last reviewed against the limits above. The architecture and SOLID posture are
**sound** (clean hexagonal boundaries, correct abstraction, no core↔UI coupling),
but several files exceed the hard size limits and must be split before we can
claim 0-debt conformance:

| File | Lines | Issue |
|------|------:|-------|
| `tests/run_tests.cpp` | 1036 | Test file; exempt from class-size rule but a candidate for per-area headers. |
| `lib/session.cpp` | 196 | OK, but `list()` mixes POSIX `opendir` with JSON — consider an `fs` helper. |
| `tui/tui_render.cpp` | 410 | Method implementations (not a class); exempt from class-size rule. |
| `tui/tui_input.cpp` | 342 | Method implementations (not a class); exempt from class-size rule. |

### Resolved
- `lib/llm.cpp` (511 → 84): split into `request_builder`, `sse_parser`,
  `http_transport`, `model_probe`, `debug_log` (+ `llm.cpp` keeps the class).
- `lib/agent.cpp` (473 → 200): after a regression that re-inlined the
  confirmation loop and tool-recovery logic, `run` is again a thin orchestrator.
  The confirmation step is `confirm_turn`; tool dispatch is the free
  `dispatch_tool_calls` (`dispatch.{h,cpp}`); parsing/sanitize/extract helpers are
  in `agent_helpers.{h,cpp}`; failure-streak + recovery steering in
  `tool_recovery.{h,cpp}`.
- `tui/tui.cpp` (1245 → 171): god-class `Tui` split into `tui.h` (declaration,
  152 lines) + `tui_render.cpp`, `tui_input.cpp`, `tui_session.cpp`,
  `tui_main.cpp`. No file defines a class >200 lines.
- `tui/widgets.cpp` (333): split into `dialog.cpp`, `form_edit.cpp`,
  `info_dialog.cpp`, `menu_select.cpp`.
- `tools/search/semantic_backend.cpp` (227 → 126): free helpers extracted to
  `semantic_index.cpp` (107 lines) + `semantic_helpers.h`.
- `tools/bash_tool.cpp` (191 → 196): `execute()` decomposed into free helpers
  `run_with_timeout` + `drain_output` in the anonymous namespace.

### Current outstanding issues
See `docs/issues.md` for the full register and `docs/fix-tracker.md` for detailed
fix plans. Key items:

| Issue | Severity | Area |
|-------|----------|------|
| Detached thread use-after-free in `chat_once` | Critical | `lib/agent.cpp` |
| HTTP transport depends on tool-cancel globals (boundary violation) | Critical | `lib/http_transport.cpp` |
| `Agent::run()` violates SRP and 10-line rule | High | `lib/agent.cpp` |
| `Agent::compress_now()` violates SRP and dupes pipeline | High | `lib/agent.cpp` |
| Tool cancel as module-level globals | High | `tools/bash_tool.cpp` |
| Tools compiled into `libagent.a` (build-layer blur) | High | `Makefile.in` |
| Tests include TUI headers (boundary violation) | Medium | `tests/run_tests.cpp` |

Method-size and branching: most methods are short, but the long `run`,
`compress_now`, and `bash_tool::execute` bodies violate the <10-line / low-branch
rule and should be decomposed first (highest leverage, lowest risk).

When refactoring to fix these, preserve behavior and keep `make test` green. Run
`make clean && make` after touching headers (see Compilation gotchas).
