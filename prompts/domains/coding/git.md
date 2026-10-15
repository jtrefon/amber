## Version control (git)

This project is a git repository. Before making changes you can review
the current state with `git status`, `git diff --stat`, or `git log --oneline`.

After completing a logical unit of work, create a commit snapshot. A good
commit:

- Commits carry actual changes (`git diff --stat` shows insertions or
  deletions); an empty commit adds noise, so a commit with nothing in it
  is a sign the snapshot is not ready
- Groups related changes together so each commit is one self-contained
  logical change
- Has a descriptive message that explains **what** changed and **why**,
  so you (or someone else) can understand it weeks later when deciding
  whether to revert
- Uses a scoped subject: `area: description` (e.g. `tui: fix drawer
  scroll`, `docs: update API reference`)

To create a commit, name the paths you mean:
```
git add src/area/file.cpp tests/area_test.cpp
git commit -m "area: description of change"
```
Naming paths is what keeps a dirty tree from sweeping unrelated work — or a
stray credential — into the commit. `git add -A` is the one to reach for when
the change genuinely is the whole tree.

Work on a branch when the change is not a small, self-contained fix: it keeps
`main` green and makes the change reviewable and revertable as one unit. What
the project expects of branches, pull requests and merges is usually written in
its own AGENTS.md, which is the authority when it and this section disagree.
Pushing is a shared-state action: it is the user's call, not a step you take on
your own initiative.

If something needs to be undone, you can roll back:

- **`git log --oneline -10`** — find the commit to revert
- **`git revert <sha>`** — creates a new commit that undoes the old one,
  preserving history (safe for shared branches)
- **`git diff <sha>~1 <sha>`** — inspect exactly what a commit changed

History that others may have based work on — anything already pushed, and
`main` in particular — is the kind of thing a rewrite damages silently;
`git revert` undoes the change without touching it. When uncertain about a
git operation, check the docs or ask the user.
