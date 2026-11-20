## Spec: Checkpoint store

### Purpose

Give a session a structured, per-turn history of its working tree so a person
can undo one turn without losing the others, and so the agent can inspect what a
turn changed. The store is the substrate; the commands and the agent tool are
separate slices.

### Where the snapshots live

| Project | Repository | Ref |
|---|---|---|
| A git checkout (including a linked worktree) | the user's repository | `refs/amber/checkpoints/<session>` |
| Not a git checkout | a private bare repo under the state directory | the same ref inside it |

The session id is sanitized to `[A-Za-z0-9_-]` (64 chars) so it is always a
valid ref component; an empty id becomes `default`. Two sessions on one
repository have independent histories.

The shadow repository is
`$XDG_STATE_HOME/amber/projects/<slug>/checkpoints/<session>.git`, with the
project as its work tree. Nothing is written inside the project: there is no
`.git` to confuse a later `git init` and no `.gitignore` entry to maintain.

### The user's work never moves

A checkpoint stages through a private index file
(`…/checkpoints/<session>.index`), writes a commit with `commit-tree`, and
updates only its own ref. It never touches the user's branch, `HEAD` or the real
index; a staged change stays staged. The ref update is compare-and-swap against
the previous checkpoint, so two writers cannot silently overwrite one history.

Commits are authored as `amber <amber@localhost>`, independent of the user's git
identity, and clearly labelled as amber-managed.

### When a checkpoint is created

`create(turn, prompt)`:

1. Stages the whole working tree into a fresh private index (`git add -A`), so
   ignored files are excluded exactly as a normal `git add -A` would exclude
   them.
2. Writes the tree.
3. Compares it with the previous checkpoint's tree. Identical means a read-only
   turn: no commit, no ref move, `nullopt` and an empty error.
4. Counts the changed files (against the parent; the first checkpoint counts
   the whole tree).
5. Writes the commit and moves the ref.

An empty project with no previous checkpoint also produces no commit.

The turn boundary that calls this — after a file-modifying turn, plus a
pre-turn snapshot — belongs to the host slice, not the store.

### The commit message is the index

```
amber: turn 42 — add JWT auth to the API
Turn: 42 | Session: abc123 | Files: 3 changed
Prompt: "add JWT auth to the API"
```

The subject's prompt is flattened and capped at 72 bytes (at a UTF-8 boundary).
The `Prompt:` value escapes `\`, `"`, newline and carriage return, so one field
is one line; `list()` unescapes it, so the prompt round-trips exactly.

### Reading

- `list()`: newest first, parsed from the commit chain on the ref. A missing ref
  is an empty history, not an error.
- `find(turn_or_sha)`: a turn number when the selector is all digits, otherwise
  a commit SHA prefix. The two kinds never fall through into each other: an
  all-digit selector that names no turn is a miss, not a SHA prefix. A prefix
  that happens to be all digits is selected by typing enough of it to include a
  hex letter (a full SHA always does).
- `diff(checkpoint)`: the unified diff the checkpoint introduced, against its
  parent commit; the first checkpoint is diffed against the empty tree.

### Failure

Git failures are exceptional for the operation, not expected states: a missing
`git`, a refused stage/commit/ref update, or a tree that cannot be read. The
store reports the git message through the caller's `error` and returns no
checkpoint. A failure before `update-ref` leaves the previous checkpoint as the
ref tip; there is no half-written history.

### Turn boundary

`CheckpointService` owns the turn rules above the store:

- `before_turn()` stages the tree and records it only when it differs from the
  tip. The snapshot takes the tip's turn number and the prompt
  `before turn N` (or `initial state` when the history is empty), with
  `Phase: pre`. Edits a person made between turns are part of the state the
  turn starts from, and the prompt names the upcoming turn.
- `after_turn(prompt)` records the turn under the tip's turn + 1 with
  `Phase: post`. A read-only turn writes nothing.
- A pre and a post snapshot can share a turn number. `find(turn)` prefers the
  post snapshot (the turn's result); the pre entry is the fallback and is
  addressed by SHA when both exist.

`CheckpointTurn` is the host adapter: the TUI worker calls it before and after
`Agent::run`, and the CLI calls it around every prompt (including the
`--wait-tasks` follow-up). It reports a status line only when a commit was
written or a git call failed, so a read-only turn stays silent.

### Restore

`CheckpointStore::restore(checkpoint)` writes the checkpoint's tree back
without moving `HEAD` or the user's real index:

1. stage the current worktree into the private index (this also gives the
   current non-ignored paths, using git's own ignore rules);
2. replace that index with the checkpoint tree and write it out with
   `checkout-index -a -f`;
3. remove the paths the checkpoint did not contain, and their empty parent
   directories. Ignored files were never in the snapshot and stay.

`CheckpointService::undo()` restores the checkpoint before the tip (the
pre-turn snapshot when the host wrote one, otherwise the previous post
snapshot), then records the restored tree as a new post checkpoint. The
recorded revert is what makes the undo itself undoable: a second undo restores
the state before the first. `rollback(selector)` does the same for any turn or
SHA prefix. A failed restore leaves the tree as it was; a successful restore
whose revert commit cannot be written reports that separately.

### Commands

The command tree owns the surface. `/session history|undo|rollback|diff` are
the canonical paths; `/history`, `/undo`, `/rollback` and `/diff` are
host-granted visible aliases that dispatch through the canonical path.

| Command | Behavior |
|---|---|
| `/history` | Newest first: turn or `before turn N`, short SHA, changed files, local time, prompt. |
| `/diff [turn\|sha]` | The checkpoint's unified diff against its parent; latest when omitted; bounded in the scrollback. |
| `/undo` | Confirm, restore the checkpoint before the tip, record the revert. Refused while the window's agent is busy. |
| `/rollback <turn\|sha>` | Confirm, restore that checkpoint, record the revert. Refused while busy. |

### Agent tool

The bundled `tool_checkpoint` plugin contributes `git_checkpoint`:

- `action=list` — the same newest-first rows, plus a structured
  `checkpoints[]` in `meta`.
- `action=diff [target=...]` — a bounded unified diff.
- `action=restore target=...` — approval-gated; restores and records the
  revert, returning both SHAs.

The tool resolves the running window's workspace root and session through
`RunScope` (`workspace_root`, `session_id`), so one tool instance serves every
window and a sub-agent's call belongs to its parent session. The TUI's turn
snapshot keys the same session (the window's stable `owner_id`), and the CLI
keys the agent's conversation id.

### Not in this slice

A key binding for `/undo`, periodic checkpoint pruning, `/bisect`, and a
proactive "that turn broke it" hint in the system prompt. The ref namespace
keeps every turn for now.

### Tests

`tests/checkpoint_test.cpp` drives real temporary repositories: a real checkout,
a non-git project (shadow), two sessions, an ignored file, a read-only turn, the
user's staged index and `HEAD` staying put, newest-first listing, find by turn
and SHA, diffs against the parent and the empty tree, an empty project, a
sanitized session id, prompt escaping and round-trip, restore with deletions and
ignored files, pre/post phase parsing, undo then redo, rollback, the
service/turn report rules, the agent tool's list/diff/restore and approval
gate, and injected git failures through a PATH shim (stage, tree write, listing,
commit, ref update, diff, and no git at all). The pty suite drives the real TUI
through a file-changing turn, `/history`, `/diff` and a confirmed `/undo`.
