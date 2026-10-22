## `watch`

`watch` runs a process or polls a PR/URL in the background, keeps a journal, and returns a report later. It is the tool for work that would otherwise hold the turn or make you poll.

### Actions

- `action=start` — start a watch and return its id at once.
- `action=list` — compact rows: id, profile, state, label, last status.
- `action=status id=...` — the current state and the last journal lines.
- `action=report id=... [detail=summary|full]` — the bounded report, evidence excerpts, and artifact paths.
- `action=cancel id=...` — stop the process group/poll and record partial output.
- `action=wait id=... timeout_s=...` — a bounded, cancellable wait; use it when the next step genuinely needs the result.

### `start` arguments

| Field | Meaning |
|---|---|
| `profile` | `process`, `pr` or `url` |
| `command` | process profile: the command line, run in the workspace |
| `pr` | PR profile: a number or URL (required in v1) |
| `url` | URL profile: an http(s) URL |
| `repo` | optional working directory for the PR/command; defaults to the window's worktree |
| `label` | short human label for `list`/status/report |
| `interval_s` | poll interval (default from `watch.conf`, 20) |
| `deadline_s` | hard stop (default from `watch.conf`, 1800) |
| `observe` | `auto` (failure/terminal), `failure` (failure only), `off` |
| `success_exit` | process profile: the exit code that means success (default 0) |

### Profiles

`process` starts a command through the host job service, polls it, and reports exit code, duration, output tail, and artifact path.

`pr` polls the PR checks, records the head SHA and attempt, and reports the terminal state. On failure it fetches the failed run log and changed-file list (bounded), writes them to artifacts, and asks the observer for a failure explanation. It requires `gh`; without it the report names the failed poll, and the watch can be cancelled.

`url` fetches the body with `curl -fsS`, compares hashes, and reports the first difference plus baseline/change artifacts. A no-change deadline is a clean `timed_out`; a fetch error is `failed`.

### Reports

A report has a headline, state (`succeeded`, `failed`, `timed_out`, `cancelled`, `unknown`), observed facts, an optional likely cause, an optional next action, confidence, an attention flag, and a compact journal update. The main agent receives the headline and id; `watch report` returns the evidence and artifact path. The full log stays in an artifact and out of the main context.

The observer is a small read-only interpreter: no tools, no shell, no writes, no policy changes. It is not a subagent and does not use the worker slots. It runs on a terminal state or a failure, not on every poll.

### Limits and approval

- `start` is approval-gated; the summary names the command/profile. The watch runs only the approved spec.
- The watch cwd is the window's worktree (or `repo`/`cwd`); process commands follow the existing workspace/approval rules.
- Watches are cheap; many can run. Observers and workers share the local model and queue; the main turn has priority.
- `cancel` stops the process group; plugin shutdown cancels every watch it owns.
- Reports and artifacts live under the project state directory; artifacts are bounded and pruned.
