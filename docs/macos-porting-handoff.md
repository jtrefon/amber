# macOS porting & CI — status and native-Mac handoff

_Last updated: 2026-09-04. Author: consolidation session + native-Mac session._

## Goal

The macOS pipeline (`build-and-test-macos` in `.github/workflows/ci.yml`) must
go green: it builds amber on `macos-latest` and (on main) produces the
Homebrew-cask tarball via `cd-macos`. The build is fixed; the two remaining
**runtime test failure classes** were reproduced and fixed on a native Mac
(see "Resolved in PR #72"). PR #72's checks are gated on this job, so the
remaining work is to verify the fix on CI and merge.

## What is already fixed (all merged to main)

The macOS **build** was broken by a series of Linux-only assumptions. All
fixed on `main`:

| Commit | Fix |
|--------|-----|
| `d93c321` | `VERSION` → `version.txt` (the repo `VERSION` file shadowed `#include <version>` on macOS's case-insensitive FS) + `PKG_CONFIG_PATH` to brew's keg-only ncurses so `./configure` finds `ncursesw` (the TUI's `mvaddnwstr`/`mvaddwstr` need wide ncurses) |
| `96c7857` | dropped the stray re-tracked `VERSION` file (only `version.txt` remains) |
| `7663f21` + `3a2b4ea` | `plugin.cpp`: add `<sstream>` + `<csignal>` (macOS libc++ doesn't pull them transitively like glibc) |
| `afefb96` | `bench/runner.cpp`: add `<sstream>` |
| `1d056d3` | `bench/template.cpp`: `WEXITSTATUS(pclose(f))` on an rvalue — macOS's `WEXITSTATUS` macro takes the address of its arg, clang rejects it; bind to a local first |
| `4b07a1d` | `window_manager.h`: forward-declare `struct Config` (was `class`, `-Wmismatched-tags`) |
| `cb3df50` | `tools/plugins/sysinfo/main.cpp`: `AF_PACKET` is Linux-only; use `AF_LINK` under `__APPLE__` |
| `5e93d82` | `lib/environment.cpp`: `sysinfo()` is Linux-only; use `sysctlbyname("hw.memsize")` under `__APPLE__` |
| `0bc1bcc` | unguard the portable SSE mock server in `tests/run_tests.cpp` (was `#ifdef __linux__`, but a later test used it unguarded) |
| `e4f9080` + `5d3672f` | add `<csignal>` to `mcp_transport_test.cpp` and `tui_tests.cpp` |
| `19e8124` | `environment_probe_collects_facts` asserted the OS contains "Linux"; now platform-aware (Darwin/Linux) |
| `602dc82` | semantic search `walk()` shelled out to GNU `find -readable` (absent on BSD/macOS find) → empty index → no hits; rewrote with `std::filesystem::recursive_directory_iterator` |
| `523e757` | `job_service_caps_output_at_one_mib` + `job_eof_daemon_is_terminated`: fixed sleeps → polling (timing on slow runners) |
| `beed491` | MCP HTTP fixture waits: 1s → 3s for the Python server statefile |

## Resolved in PR #72 (native-Mac session)

Both remaining failure classes were reproduced on a native Mac and fixed.

### 1. MCP HTTP fixture tests — Python server never wrote its statefile

The tests launched a fixture via
`std::system("python3 tests/fixtures/mcp_http_server.py ... &")` with stderr
suppressed, so whatever failed on the macOS runner was invisible. Rather than
rely on the runner's Python at all, the test suite no longer depends on
Python:

- `tests/fixtures/mcp_echo.cpp` — stdio JSON-RPC server (modes: default,
  `stderr`, `boom`), behavior-identical to the old `mcp_echo.py`.
- `tests/fixtures/mcp_http.cpp` — HTTP server (modes: `echo`, `sse`,
  `session`) writing the `PORT:/PID:` statefile, mirroring
  `mcp_http_server.py`. A header-parse bug surfaced during the port (the
  final header line lost its value when it had no trailing CRLF) was fixed.
- `tests/fixtures/mcp_ignore_sigterm.cpp` — SIGTERM-ignoring daemon writing
  its PID file, mirroring `mcp_ignore_sigterm.py`.

The fixtures are standalone C++ executables built by `make test` (no libagent
link; header-only nlohmann/json), and the Python scripts were deleted. Tests
invoke them as `tests/fixtures/mcp_echo` instead of `python3
tests/fixtures/....py`, so **`make test` runs with zero Python dependency**.
The external-plugin fixture (`tests/plugins/fake_plugin.py`) is separate and
tracked as follow-up.

### 2. `job_eof_daemon_is_terminated` — exit code race

The test read `exit_code == -1` instead of `128 + SIGKILL` because the job
state machine had two writers to `exit_code` without the reap consistently
winning: `begin_kill()` published `state = Killed` and `exit_code = -1`
optimistically, and `reap_after_eof()` overwrote the exit code with the real
`128 + WTERMSIG` only after the `waitpid` reap — a window large on macOS
(slow process-group teardown) during which a poller observes the stale `-1`.

**Fix** (`lib/job.cpp` + `include/agent/job.h`): `begin_kill()` now only
signals the process group and latches `kill_done_`; the reader thread's
single `finalize()` writer sets the exit code **before** publishing the state,
under one lock. A consumer can never observe `Done`/`Killed` with a stale
exit code, on any platform; the reader loop's time-out path was deduplicated
onto the same `finalize()`.

## Related PR / branch state

- **PR #72** (`fix/macos-consolidated-review` → main): carries the two UI
  fixes **and** the two macOS runtime fixes above:
  - `feat: state-aware working indicator verbs + fix spinner mojibake`
  - `fix: drawer arrows clobber input on exact-command descend; Alt+number switch`
  - MCP test fixtures rewritten in C++ (no Python test dependency)
  - job EOF reap finalizes the exit code before publishing state
  Its checks are gated on `build-and-test-macos`; merging is blocked until
  the macOS Test step is green.
- All the small-model macOS PRs (#66, #67, #69, #70, #71) are **closed as
  superseded**; their content is on main or in #72.
- `main` carries all the macOS build fixes listed above.

## How to continue

1. Check out PR #72 (`fix/macos-consolidated-review`).
2. On a Mac: `brew install ncurses` (keg-only; CI installs it), then
   `PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig" ./configure` and
   `make clean && make && make test`. The suite no longer needs Python.
3. Push; macOS CI should go green; merge #72 (or fold it into further macOS
   work).
4. Consider adding a macOS-specific test exercising `sysctlbyname`
   (`hw.memsize`) and the semantic-search filesystem walk, so the portability
   fixes stay covered on the platform that needs them.
5. Follow-up (independent of this PR): replace the Python external-plugin
   fixture (`tests/plugins/fake_plugin.py`) and the Python benchmark oracle
   (`bench/template.cpp`) so `make test`/`make bench` never require a Python
   runtime.