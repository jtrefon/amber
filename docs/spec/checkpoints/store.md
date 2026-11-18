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
  a commit SHA prefix.
- `diff(checkpoint)`: the unified diff the checkpoint introduced, against its
  parent commit; the first checkpoint is diffed against the empty tree.

### Failure

Git failures are exceptional for the operation, not expected states: a missing
`git`, a refused stage/commit/ref update, or a tree that cannot be read. The
store reports the git message through the caller's `error` and returns no
checkpoint. A failure before `update-ref` leaves the previous checkpoint as the
ref tip; there is no half-written history.

### Not in this slice

`/history`, `/undo`, `/rollback`, `/diff`, the TUI turn trigger, the
`git_checkpoint` agent tool, and restore. Those are the remaining slices of
#441; the store's `ref()`, `list()`, `find()` and `diff()` are the seam they
consume.

### Tests

`tests/checkpoint_test.cpp` drives real temporary repositories: a real checkout,
a non-git project (shadow), two sessions, an ignored file, a read-only turn, the
user's staged index and `HEAD` staying put, newest-first listing, find by turn
and SHA, diffs against the parent and the empty tree, an empty project, a
sanitized session id, prompt escaping and round-trip, and injected git failures
through a PATH shim (stage, tree write, listing, commit, ref update, diff, and no
git at all).
