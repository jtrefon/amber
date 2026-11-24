# CLI agent journeys

Each file here is one end-to-end journey through the real `amber-cli`:
`make agent-e2e` runs `amber-bench run --driver binary --suite e2e` against the
shared loopback endpoint. The in-process reference run supplies the expected
tool sequence; the CLI run must match the final answer, workspace, exit code,
token floors and per-call status.

Fields beyond the common scenario schema:

- `setup.executables`: paths under the workspace to mark executable after
  `setup.files` writes them. `@ROOT@` in a setup file body, executable path or
  shell command expands to the checkout root, so a scenario can point at
  `tests/fixtures/mcp_echo` or copy a plugin script without an absolute path in
  the JSON.
- `enable_plugins`: external project plugins (`.amber/plugins/<id>`) the run
  enables. The in-process runner activates them with `apply_state` (no persisted
  choice); the CLI gets one `--enable-plugin <id>` per entry.
- `provider` / `flavor`: the CLI-tier provider preset and wire protocol. They
  are written into the journey's `--config`, and a non-OpenAI flavor uses the
  endpoint root (the versioned dialect adds `/v1` or `/v1beta`). The
  `request_contract` can assert the path.

- `session`: the `--session NAME` the CLI uses. It persists the conversation in
  the project state so a second process can resume it.
- `resume`: run each turn as a separate CLI process against `session`; the
  endpoint sees the saved context in the second process's first request.
- `cancel_marker`: send `SIGINT` to the CLI when this text appears on its
  stderr. The journey asserts the normal cancel path (exit 130, no partial
  artifact).
- `request_log_contains`: strings that must appear in some chat request body
  (context carry, a worker report, a skill body).
- `expected_tools`: the CLI's full tool sequence when it includes calls the
  in-process parent does not record (a subagent's worker tools).
- `endpoint_replies`: the endpoint FIFO when it differs from `fake_replies`,
  e.g. parent and worker replies interleaved on one wire.
- `cli_expected_workspace`: workspace assertions the CLI tier makes after
  behaviour the in-process reference run cannot reproduce (a cancelled tool).
- `compression_threshold`, `compression_min_turns`,
  `compression_cooldown_turns`, `compression_target_pct`, `context_size`: passed
  to the CLI as `--config` and applied to the in-process run, so the gate fires
  at the same point in both tiers.

The surface journeys add two cases each for skills
(`e2e-skill-activation`, `e2e-skill-missing-recovery`), MCP
(`e2e-mcp-tool-call`, `e2e-mcp-missing-tool-recovery`) and external plugins
(`e2e-plugin-tool-call`, `e2e-plugin-failure-recovery`), the Anthropic and
Gemini dialect journeys (`e2e-provider-anthropic`, `e2e-provider-gemini`), and
the coding library's `library_lookup` round trip (`e2e-coding-library`). The
workspace tools add `todowrite` (`e2e-tool-plan`), `process_start`
(`e2e-tool-process`), `process_start` + `tasks` (`e2e-tool-tasks`), the project
index (`e2e-tool-project-index`) and the watch tool (`e2e-tool-watch`); the
latter three exit 3 because a background task is deliberately still live, which
is the CLI's documented delivery contract. The failure paths assert that an
unknown skill/tool or a plugin failure is a tool error the model can recover
from, not a crash. The TUI suite covers the same skill, MCP, plugin, provider,
filesystem, worktree, tasks, project, search/web and watch surfaces through the
real pty client.

A failed journey keeps its workspace (`--keep-failures`) with `telemetry.json`,
`requests.json`, `out.txt` and `err.txt`.
