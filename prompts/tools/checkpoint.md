## git_checkpoint

Amber snapshots the working tree before and after each file-changing turn and
keeps those snapshots on a private ref. The history is the harness's memory of
what it changed, available to the agent as a tool.

A snapshot is a commit. Its message carries the turn number, the session, the
changed-file count and the prompt, so a list is enough to find the turn that
introduced a change. The user's branch, `HEAD` and real index stay where they
were: the snapshot stages through a private index and updates only
`refs/amber/checkpoints/<session>`.

### Actions

- `action=list` — the newest first: turn, short SHA, changed-file count, time
  and prompt. A `before turn N` row is the state the turn started from; a
  `turn N` row is what the turn produced.
- `action=diff [target=...]` — the unified diff a checkpoint introduced. The
  latest checkpoint when `target` is omitted. Output is bounded; the metadata
  says when it was truncated.
- `action=restore target=...` — put the working tree back to a turn number or
  SHA prefix, then record the revert as a new checkpoint. Approval-gated,
  because it overwrites uncommitted files. The revert being a checkpoint means
  a restore can itself be restored.

### When to use it

When a test or a run regresses, `list` narrows the candidate turns and `diff`
shows what one changed, without reading the whole conversation back. A
`restore` is for a turn whose change should leave the tree; inspect the diff
first, and prefer the narrowest target that removes the unwanted change.

The history is per session. Two windows on one repository keep independent
checkpoint refs, and a window's worktree keeps its own tree.
