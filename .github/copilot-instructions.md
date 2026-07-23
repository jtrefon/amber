# Copilot Code Review Instructions — amber (cpp-agent)

## Repository context

amber is a C++17 AI agent harness with a hexagonal (ports & adapters) architecture.
The repository AGENTS.md documents all engineering principles, coding standards, and
the fix workflow. These instructions supplement it for automated review.

## Review criteria — every PR must satisfy

### SOLID conformance
- No new SRP violations: classes should have one reason to change.
- Dependency direction is correct: `lib/` must never `#include` from `tui/`, `src/`,
  or `tools/` (except tool interface headers in `include/agent/`).
- Narrow interfaces: `Tool`, `SearchBackend`, `AgentHooks` are the patterns.

### Size limits
- Classes ≤200 lines. Methods ≤10 lines with minimal branching.
- Flag extraction opportunities: loops, parsing, and branching should be named helpers.

### Zero dead code
- No commented-out code, no stubs, no speculative branches.
- If functionality is removed, remove it entirely — do not leave dead paths.

### Red → Green test sequence
- Bug fixes and features must start with a failing test commit, then a fix commit.
- If a single commit combines both, flag it for explanation.

### Error handling
- Tools return errors via `ToolResult{false, "", error_msg}` — never throw from execute().
- Library functions may throw `std::runtime_error` for transport/config failures only.
- Assertions are for invariants only, never for input validation.

### Coding standards
- RAII: `unique_ptr` for exclusive ownership, `shared_ptr` only for genuinely shared ownership.
- `noexcept` on pure accessors, trivial getters, and `Tool::name()` / `is_read_only()`.
- Const-correctness: mark member functions and parameters `const` where possible.
- Rule of Five/Zero: prefer implicit special members; if any is user-defined, declare all five.

### Hexagonal boundaries
- Ports (interfaces) live in `include/agent/`, adapters live in `tools/` or `tui/` or `src/`.
- The core (`lib/` + `include/agent/`) must not reference UI, ncurses, or main().

## Focus areas

- **clang-tidy** and **cppcheck** warnings are blockers. PRs introducing new warnings
  must be rejected regardless of correctness.
- Review for unnecessary `#include` chains, especially in headers (prefer forward
  declarations).
- Ensure `std::thread::detach()` is never used — use tracked futures or join on
  destruction.
- Tool implementations must be stateless or store cancellation tokens via constructor
  injection — no module-level globals.
