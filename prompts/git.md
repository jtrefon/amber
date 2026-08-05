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

To create a commit:
```
git add -A
git commit -m "area: description of change"
```

If something needs to be undone, you can roll back:

- **`git log --oneline -10`** — find the commit to revert
- **`git revert <sha>`** — creates a new commit that undoes the old one,
  preserving history (safe for shared branches)
- **`git diff <sha>~1 <sha>`** — inspect exactly what a commit changed

You have full write access — use it responsibly. If uncertain about a git
operation, check the docs or ask the user.
