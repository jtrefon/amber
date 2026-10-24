# Spec: Session usage ledger

### Purpose

The status bar's `↑/↓` meter must answer "what did this session cost?", not
"what did the last request cost". Work that a chat causes — sub-agents,
compression, watches — is part of its spend and must be visible.

### Ownership

Host-owned (`tui::UsageLedger`, `tui/session_usage.h`). The core only reports:
`AgentHooks::on_usage` carries a `UsageSample` tagged with a `UsageSource`;
`SubAgentExecutor` records per-task totals and forwards samples; the compressor
reports both LLM calls through `CompressionObserver::on_llm_usage`; the watch
plugin accumulates observer spend per owner.

### Model

| Source | What it counts |
|---|---|
| `main` | this conversation's own requests |
| `subagents` | every task it spawned (each task also keeps its own totals) |
| `compression` | the classify and extract calls |
| `watch` | watch-observer calls attributed to the owning window |
| `inherited` | the baseline a fork started from |

- `tree()` = the five sources summed; `measured` is true when any source has a
  server-reported count.
- Counters are `long` (64-bit on the supported platforms); formatting uses
  `agent::compact::count` (k/m/b/t), so a readout never widens with magnitude.
- `↑` is the sum of per-request prompt tokens; `↓` is the sum of completions.
  Neither is derived from the context size.
- The bar shows the active chat window's tree total. A resource viewer shows
  that resource's own usage (a task viewer reads `SubAgentTaskInfo`).

### Persistence and reset

- Session meta stores each source **and** the tree under the legacy
  `session_prompt_tokens` / `session_completion_tokens` keys, so an older build
  can still read a newer session and vice versa.
- A fork inherits the source's tree as `inherited`; new spend accrues to the
  fork's own sources.
- `/usage reset` zeroes the active session tree, children and inherited
  baseline included. Compression and `/session new` never reset spend.
- `/usage show` prints main, sub-agents (aggregate and per task), compression,
  watches, the tree total, and each open chat window's total when more than one
  exists.

### Scenarios

- **UL-01** A session that spawns N sub-agents and compresses reports all of
  their input/output tokens in the tree total.
- **UL-02** A task viewer shows that task's own usage
  (`subagent_usage_is_recorded_and_forwarded`).
- **UL-03** Compression reports one sample per call, classify and extract
  (`compression_pipeline_reports_usage_per_request`).
- **UL-04** Watch spend is attributed to the owner and drained once
  (`watch_observer_usage_is_attributed_to_the_owner`).
- **UL-05** Totals survive save/restore; a legacy meta still loads as `main`
  (`usage_ledger_round_trips_session_meta`,
  `usage_ledger_reads_legacy_session_keys_as_main`).
- **UL-06** Reset zeroes the whole tree, inherited included, and only on the
  explicit command (`usage_ledger_tree_sums_sources_and_resets`,
  `usage_show_and_reset_are_reachable`).
