## Version control (git)

This project is a git repository. Before making changes you can review
the current state with `git status`, `git diff --stat`, or `git log --oneline`.

A commit is a snapshot, and its subject is the index you will scan later. Make
one snapshot per completed step — not per edit, not one for the whole session —
and keep it to one area: `area: description` says what changed, and a change
spanning two unrelated areas is two commits, not one wide key. An empty snapshot
adds noise, so a commit with nothing in it is a sign it is not ready.

To create a commit, name the paths you mean:
```
git add src/area/file.cpp tests/area_test.cpp
git commit -m "area: description of change"
```
Naming paths is what keeps a dirty tree from sweeping unrelated work — or a
stray credential — into the commit. `git add -A` is the one to reach for when
the change genuinely is the whole tree.

Work on a branch when the change is not a small, self-contained fix. In a window
workspace (a detached worktree) create one before the first commit:
`git switch -c fix/<topic>` — the window will not discard commits that no branch
reaches. The project's AGENTS.md outranks this section on branches, pull
requests and merges; pushing is the user's call, not a step you take alone.

If something needs to be undone, inspect it first, then roll back:
- **`git log --oneline -10`** — find the commit
- **`git diff <sha>~1 <sha>`** — see exactly what it changed
- **`git revert <sha>`** — a new commit that undoes it, preserving shared history
- **`git reset --soft <sha>`** — only for a local, unpushed snapshot; it keeps
  the work, and history others may have is not yours to rewrite

When uncertain about a git operation, check the docs or ask the user.
