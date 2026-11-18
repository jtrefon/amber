# CLI agent journeys

Each file here is one end-to-end journey through the real `amber-cli`:
`make agent-e2e` runs `amber-bench run --driver binary --suite e2e` against the
shared loopback endpoint. The in-process reference run supplies the expected
tool sequence; the CLI run must match the final answer, workspace, exit code,
token floors and per-call status.

Fields beyond the common scenario schema:

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

A failed journey keeps its workspace (`--keep-failures`) with `telemetry.json`,
`requests.json`, `out.txt` and `err.txt`.
