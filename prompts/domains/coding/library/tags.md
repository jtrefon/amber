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

## Domain

- `cpp` — C++: language core, toolchain, build
- `cmake` — CMake: targets, presets, packaging
- `make` — Make and Makefiles
- `rust` — Rust: cargo, crates, editions
- `go` — Go: modules, tooling
- `node` — Node.js: npm and pnpm workspaces
- `typescript` — TypeScript: tsconfig, types
- `python` — Python: packaging, virtualenvs
- `php` — PHP itself
- `composer` — composer dependencies
- `dotnet` — .NET: SDK, csproj, solutions
- `docker` — container images and the docker CLI
- `dockercompose` — docker compose: services, networks, volumes
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

- `debugging` — a failure that resists reading the code
- `testing` — designing, running or trusting tests
- `sanitizers` — memory and lifetime faults
- `lint` — static analysis, warnings and suppressions
- `tooling` — the tools around the code
- `build` — build systems, linking, compilation
- `templates` — generic programming, concepts, constraints
- `errors` — how a function reports failure
- `abi` — what a binary agrees on with its consumers
- `perf` — measuring and improving speed
- `concurrency` — threads, async, cancellation
- `async` — asynchronous work specifically
- `memory` — allocation, lifetime, ownership
- `ownership` — who owns a value, in any language
- `containers` — running things in containers
- `packaging` — building a releasable artefact
- `vendoring` — pinning, fetching and vendoring dependencies
- `observability` — logs, traces, telemetry
- `gdb` — debuggers and their scripting
