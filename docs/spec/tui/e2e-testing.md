# Spec: Agent end-to-end testing (pty, CLI and TUI journeys)

Status: implemented (#634, #635, #636, #639, #645, #640; gated by the
`agent-e2e` and `tui-agent-e2e` CI jobs).

Two layers answer different questions:

- the **pty regression suite** (`tests/tui_pty_test.cpp`) pins the interface:
  keys, dialogs, drawers, redraws;
- the **agent journeys** pin the thing a person does with it: prompt → tool →
  edit → answer, through `main()`, the plugin runtime and the real tools.

No pixel-perfect screen goldens. A journey asserts semantic text, workspace
state, tool calls and the exit path.

## The pty harness

`tests/pty_harness.h` / `.cpp` own the shared driver:

- `make_isolated_root()` creates a private workspace under the system temp dir
  and sets `AMBER_WORKSPACE` plus the XDG config/state/cache roots. It is
  called before the app starts, so a journey never reads developer state.
- `Tui::start(binary, root)` forks `amber` on a pty. `Tui::raw` is the byte
  stream, `text()` strips ANSI, `pump(ms)` drains, and `wait_for(marker, cap)`
  polls the rendered text. `send()` writes keys.
- `wait_ready(tui, workspace_name)` waits for the startup marker (completions
  loaded, workspace in the status line), not a fixed sleep.
- With `AMBER_EXIT_READY` set, the app writes the exit-ready marker as the last
  thing it does. `Tui::stop()` sends the app's quit path and waits for it; a
  session that has to be SIGKILLed fails the suite, because a killed child never
  flushes coverage. The whole quit path is bounded by
  `AMBER_PTY_EXIT_TIMEOUT_MS` (default 60 s).

The macOS job repeats the TUI journeys twice (`make tui-agent-e2e REPEAT=2`):
each pass is a fresh process, so a plugin-runtime or model-cache static that
survives in-process cannot hide a state leak.

## TUI journeys

`tests/tui_agent_e2e_test.cpp` owns a `Journey` helper: an isolated root, a
loopback `bench::ScriptedEndpoint`, and the pty session. `start()` accepts a
scripted deque or a responder (so a journey can inspect the request and answer
per path), an `extra_settings` string, a `prepare` hook that runs before startup
(skills/MCP/plugins) and `root_api_base` for a versioned dialect.

The current journeys are:

| Journey | What it proves |
|---|---|
| `read-edit-verify` | prompt → read → patch → read back → answer; workspace and request count |
| `context` | the second turn sees the first turn's tool result |
| `approval-denied` | a denied bash call still allows the write fallback |
| `approval-allowed` | the dialog's allow path runs the command and writes its marker |
| `cancel` | `/stop` cancels a slow tool; no partial artifact; the next turn works |
| `background-tab` | a worker window keeps its own result; it does not leak into tab 1 |
| `restart` | a second process resumes the saved session and its context |
| `subagent` | a delegated worker reports to its owner; `/tasks list` names it |
| `skill` | an authored `SKILL.md` body reaches the model after `read_skill` |
| `mcp` | a real stdio MCP server auto-connects; its tool runs; `/mcp list/show` |
| `plugin` | a project external plugin is enabled, its tool runs, output on screen |
| `provider` | the Anthropic path `/v1/messages` is used and the raw reply renders |
| `compression` | `/compress` accepts a queued prompt while the classifier runs |

Run them with:

```sh
make tui-agent-e2e
make tui-agent-e2e REPEAT=2                 # the macOS shape
AMBER_TUI_JOURNEY=plugin ./tui_agent_e2e_test ./amber
```

## CLI journeys

`bench/scenarios/e2e/*.json` are the CLI journeys. They are ordinary scenarios
plus `endpoint_replies`/`expected_tools`/`request_log_contains` where the CLI
wire differs from the in-process reference. `bench/binary_driver.cpp` runs each
scenario twice:

1. an **in-process reference** run with the scenario's fake/policy client,
   which supplies the expected tool sequence and statuses;
2. the real `amber-cli` on a loopback endpoint, in an isolated workspace with
   its own XDG roots.

The CLI run must match the final answer, exit code, workspace, token floors,
per-call statuses and tool sequence, and a `request_contract` checked at the
wire (`path`, `tools_include`/`exclude`, `system_prefix_stable`,
`no_system_after_first`). `request_log_contains` asserts a value crossed the
model boundary (a worker report, a skill body, an MCP/plugin result).
`cli_expected_workspace` covers behaviour the in-process reference cannot
reproduce (a cancelled tool leaves no artifact).

```sh
make agent-e2e
make agent-e2e-check     # journeys + the committed token/call/wall baseline
```

`tools/agent_e2e_check.py` fails when a baseline journey is missing or now
fails, when prompt+completion tokens or tool calls grow beyond 10%, or when
wall time exceeds `max(2x + 500 ms, +2000 ms)`. A new journey is additive and
red if it fails. The baseline is regenerated only after review with
`make agent-e2e-baseline`.

## Adding a CLI journey

1. Add `bench/scenarios/e2e/<name>.json` with `setup`, `prompt`, `fake_replies`
   (and `endpoint_replies` if the CLI wire differs), `oracle`, `checks`,
   `expected_workspace`, `budget` and `platforms`.
2. Run it red: `amber-bench run --driver binary --scenario <name>`. A command
   that cannot reproduce the expected failure is a harness gap, not a test.
3. Fix the harness/engine, then make it green. `make agent-e2e` runs the whole
   `e2e` suite; `--keep-failures` leaves `telemetry.json`, `requests.json`,
   `out.txt` and `err.txt` for a red journey.
4. Run `make agent-e2e-check`; if the journey legitimately changes the token,
   call or wall profile, regenerate the baseline and say why in the PR.
5. The same scenario is part of the `agent-e2e` job; no local-only journey.

For a long-horizon repository journey use `bench/scenarios/repo-long/` and
`make long-horizon-check` (see `bench/scenarios/repo-long/README.md`).

## Adding a TUI journey

1. Add a `journey_<name>` function to `tests/tui_agent_e2e_test.cpp`. Start a
   `Journey` with a scripted endpoint; use `prepare` for skills/MCP/plugins and
   `root_api_base` for a versioned dialect.
2. Wait for semantic markers (`wait_ready`, `wait_for`), never fixed sleeps.
   Approve a dialog by its rendered text; cancel with `/stop`; assert the
   workspace and exit-ready path through `journey.finish`.
3. Run it alone with `AMBER_TUI_JOURNEY=<name> ./tui_agent_e2e_test ./amber`,
   then `make tui-agent-e2e` and `make tui-agent-e2e REPEAT=2`.
4. A TUI journey is not a pty interface test: if it needs a new keybinding or
   dialog, add the interface test in `tests/tui_pty_test.cpp` too.

The CI jobs (`agent-e2e`, `tui-agent-e2e`) are in `ci-gate`; a red journey
fails the merge. `make agent-eval` is the local umbrella for the CLI, policy
matrix, trace and long-horizon baselines; `make preflight` runs it and the TUI
journeys.
