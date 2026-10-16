trigger: when a hotfix is needed while the current branch is half-finished, or a long build needs a second checkout
tags: #git
weight: 45
verified: 2026-10
reference: https://git-scm.com/docs/git-worktree
sources: model knowledge 2026-10; pending review against Pro Git Ch. 7 / git-worktree man page

A worktree is a second working directory attached to the same repository: another
branch checked out somewhere else, sharing one object database and one set of refs.

- **The use case**: a hotfix on `main` while a feature branch is mid-rebase, or a
  release build that must not be disturbed by the next edit. `git stash` solves a
  smaller version of this; a worktree does not disturb anything.
- **`git worktree add ../hotfix -b hotfix`** creates the directory and the branch;
  `git worktree list` shows them with their branches; `git worktree remove <dir>`
  removes one (and refuses if it has changes until you force it).
- **One branch, one worktree**: checking out the same branch in two worktrees is
  refused, because the index would be shared and the two would fight.
- **A worktree is not a clone**: commits, branches and stashes are all in the same
  repository, so a push in one is visible in the other immediately.
- **The stash is shared, which is a hazard in a window workspace**: it lives in the
  common repository, so a stash made in one worktree is visible from — and poppable
  in — every other one. In a window's workspace prefer a throwaway WIP commit on a
  branch to a stash: a sibling window can pop it, and the workspace lives under
  `$TMPDIR`, which is volatile by design.
- **Cheap and disposable**: it is how a review of someone else's branch or a
  bisect run happens without disturbing your own tree — `git bisect` in one
  worktree while you keep working in another.
- **CI and tooling see a normal directory**: the `.git` file inside points back to
  the main repository, so anything that reads git state works unchanged.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
printf 'base\n' > file.txt && git add . && git commit -qm "base"
git worktree add -q ../hotfix -b hotfix
test -f ../hotfix/file.txt
(cd ../hotfix && printf 'fix\n' > fix.txt && git add . && git commit -qm "apply the hotfix")
git log --oneline hotfix | grep -q "apply the hotfix"
git worktree list | grep -q hotfix
git worktree remove ../hotfix
