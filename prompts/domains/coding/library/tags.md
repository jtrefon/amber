# The library's tag vocabulary

Every tag an entry carries, and every tag the project scanner can propose, comes
from this list. A test checks both directions — the entries against the list, and
the scanner's emitted tags against the list — because the failure this prevents is
silent: a scanner proposing `#dockercompose` while the entries are tagged
`#docker` selects nothing, and the working index simply stays empty.

Two kinds of tag. **Domain** is what the project *is*: a language, a tool, a
framework. **Need** is what the work *needs*: a situation the agent is in. Most
domain tags come from a manifest the scanner reads; a tag that has no entry yet
selects nothing, so an entry arrives with the tag rather than ahead of it.

A tag line may carry `also:` — the words a reader uses for that tag. Retrieval
expands them, so a question about OOP reaches every `design` entry without each
entry repeating the acronym.

## Domain

- `cpp` — C++: language core, toolchain, build. also: cplusplus
- `cmake` — CMake: targets, presets, packaging
- `make` — Make and Makefiles
- `rust` — Rust: cargo, crates, editions
- `go` — Go: modules, tooling
- `node` — Node.js: npm and pnpm workspaces
- `typescript` — TypeScript: tsconfig, types
- `python` — Python: packaging, virtualenvs
- `php` — PHP itself
- `composer` — composer dependencies
- `dotnet` — .NET: SDK, csproj, solutions. also: csharp
- `docker` — container images and the docker CLI. also: container image
- `dockercompose` — docker compose: services, networks, volumes. also: compose
- `next`, `react`, `angular`, `vue`, `svelte` — front-end frameworks
- `tailwind`, `bootstrap` — CSS frameworks
- `vitest`, `jest` — JavaScript test runners
- `phpunit` — the PHP test runner
- `laravel`, `symfony` — PHP frameworks
- `wordpress`, `woocommerce` — WordPress and its commerce plugin
- `django`, `fastapi`, `flask` — Python web frameworks
- `pytest` — the Python test runner
- `efcore` — Entity Framework Core

## Need

- `debugging` — a failure that resists reading the code. also: debug, bug
- `testing` — designing, running or trusting tests. also: test, unit test, tdd
- `sanitizers` — memory and lifetime faults. also: asan, ubsan, tsan, use-after-free, memory error
- `lint` — static analysis, warnings and suppressions. also: static analysis, clang-tidy, cppcheck
- `tooling` — the tools around the code. also: toolchain, tools
- `build` — build systems, linking, compilation. also: compile, compilation, build system
- `templates` — generic programming, concepts, constraints. also: generics, concepts
- `errors` — how a function reports failure. also: error handling, exceptions, expected
- `abi` — what a binary agrees on with its consumers. also: binary interface
- `io` — sockets, readiness, async I/O. also: epoll, io_uring, sockets
- `git` — history, archaeology, recovery, collaboration. also: version control, commit, branch, rebase, bisect
- `perf` — measuring and improving speed. also: performance, optimize, optimisation, profile, profiling
- `design` — how code is shaped: principles, SOLID, trade-offs. also: oop, object-oriented, solid, design principles
- `patterns` — recurring shapes and when they pay. also: pattern, design pattern, gang of four, gof
- `architecture` — how a system's parts are arranged and depend. also: microservices, monolith, event sourcing, cqrs, hexagonal, layering
- `concurrency` — threads, async, cancellation. also: thread, race, data race
- `async` — asynchronous work specifically. also: asynchronous, coroutine
- `memory` — allocation, lifetime, ownership. also: allocation, lifetime
- `ownership` — who owns a value, in any language. also: own, owner
- `containers` — running things in containers. also: container
- `packaging` — building a releasable artefact. also: install, release, distribution
- `vendoring` — pinning, fetching and vendoring dependencies. also: dependencies, conan, vcpkg
- `observability` — logs, traces, telemetry. also: telemetry, tracing, logs
- `gdb` — debuggers and their scripting. also: debugger, lldb
