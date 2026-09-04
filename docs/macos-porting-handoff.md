# macOS porting & CI — status and native-Mac handoff

_Last updated: 2026-09-04. Author: consolidation session._

## Goal

The macOS pipeline (`build-and-test-macos` in `.github/workflows/ci.yml`) must
go green: it builds amber on `macos-latest` and (on main) produces the
Homebrew-cask tarball via `cd-macos`. The build is fixed; a small set of
**runtime test failures** remain that need investigation on a **native Mac**
(not 15-minute blind macOS-CI cycles).

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

## Remaining macOS failures (need native-Mac investigation)

After all the above, `build-and-test-macos` (Test step) still reports these
failures. Reproduced from the last full CI run (run id 33857666352):

### 1. MCP HTTP fixture tests — Python server never writes its statefile

Failing tests (all in `tests/mcp_transport_test.cpp` via `HttpFixture`):
- `mcp_http_json_roundtrip`
- `mcp_http_sse_streaming_response`
- `mcp_http_session_expiry`
- and `mcp_manager_http_connect` (`tests/mcp_config_test.cpp:238`)

Symptom: `wait_for_file(statefile)` fails — the statefile at
`/tmp/mcp_http_<mode>.txt` never appears even after the wait was raised to 3s.

Mechanism (`tests/mcp_transport_test.cpp:134-140`):
```cpp
std::string cmd = "python3 tests/fixtures/mcp_http_server.py " +
                  statefile + " " + mode + " >/dev/null 2>&1 &";
ASSERT(std::system(cmd.c_str()) == 0);
ASSERT(wait_for_file(statefile));
```
The fixture stderr is suppressed (`2>/dev/null`), so **we cannot see why
python3 fails to start** on the macOS runner. The fixture script itself
(`tests/fixtures/mcp_http_server.py`) is portable stdlib Python (binds
`127.0.0.1:0`, writes `PORT:<port>\nPID:<pid>\n`).

**Native-Mac investigation checklist:**
1. Run `make test` on a Mac and capture whether `python3` exists / which one
   (`which python3`, `python3 --version`). The GitHub `macos-latest` runner's
   `python3` may differ from a dev Mac's.
2. Run the fixture manually:
   `python3 tests/fixtures/mcp_http_server.py /tmp/t.txt echo` — does it write
   the statefile?
3. Check the test's CWD when `make test` runs — `tests/fixtures/...` is a
   relative path; if the binary runs from a different CWD the script isn't
   found (and stderr is swallowed).
4. **Recommended fix direction**: capture the fixture's stderr to a temp file
   and `ASSERT`-report it on failure (so CI shows the real error), and/or
   resolve the fixture path absolutely from the test binary's location.
   Also confirm this isn't a port/statefile collision from a prior test's
   server not being cleaned up (the same family flaked once on Linux).

### 2. `job_eof_daemon_is_terminated` — exit code race

`tests/run_tests.cpp` (~line 4346). The test starts
`exec 1>&- 2>&-; sleep 30`, waits for the EOF-reap path to force-kill it, and
asserts:
```cpp
ASSERT(info.state != agent::JobState::Running);   // passes now
ASSERT_EQ(info.exit_code, 128 + SIGKILL);          // FAILS: -1 != 137
```
`exit_code` reads **-1** instead of **137**. In `lib/job.cpp`, `begin_kill()`
(force-kill path) sets `exit_code_ = -1` and state `Killed`, while
`reap_after_eof()`/`reader_loop()` set `exit_code_ = 128 + WTERMSIG(status)`
when the `waitpid` reap observes the signal. On macOS the kill lands but the
reap's exit-code assignment races the test's read — the state machine has two
writers to `exit_code_` without the reap consistently winning.

**Native-Mac investigation checklist:**
1. Reproduce on a Mac: does `sleep 30` (the child) get SIGKILLed as a process
   group on macOS the same way? macOS `kill(-pid, SIGKILL)` semantics differ
   subtly from Linux (no `kill(-1)` surprises, but process-group signaling
   from a non-group-leader parent can differ).
2. Examine `lib/job.cpp` `begin_kill()` vs `reap_after_eof()`: the race is
   that `begin_kill()` sets `exit_code_ = -1` optimistically; the reap should
   overwrite it with the real signal status but may run before/after
   nondeterministically. **Recommended fix direction**: in the killed path,
   have the reader/reaper own the final `exit_code_` write (e.g. after
   `kill_process_group`, `waitpid` once and set `128 + WTERMSIG`), rather than
   hardcoding -1, or make the test accept either `-1` or `137` (the state
   already proves it was force-killed).

## Related PR / branch state

- **PR #72** (`fix/macos-consolidated-review` → main): carries the two UI
  fixes that are independent of the macOS runtime tests:
  - `feat: state-aware working indicator verbs + fix spinner mojibake`
  - `fix: drawer arrows clobber input on exact-command descend; Alt+number switch`
  Its checks are gated on `build-and-test-macos`; merging it is blocked until
  the macOS Test step is green OR the repo's required checks are adjusted.
- All the small-model macOS PRs (#66, #67, #69, #70, #71) are **closed as
  superseded**; their content is on main or in #72.
- `main` carries all the macOS build fixes listed above.

## How to continue on a native Mac

1. Check out `main` (has all build fixes) or PR #72.
2. Run `make clean && make && make test` locally on macOS.
3. Fix the two failure classes above using the checklists.
4. Push to a branch; the macOS CI should go green; merge #72's UI fixes (or
   fold them into the macOS work).
5. Consider adding a macOS-specific test that exercises `sysctlbyname`
   (`hw.memsize`) and the semantic-search filesystem walk, so the portability
   fixes are covered on the platform that needs them.
