trigger: when undoing a commit, unstaging work, or deciding whether --hard is safe
tags: #git
weight: 60
verified: 2026-10
reference: https://git-scm.com/docs/git-reset
sources: model knowledge 2026-10; pending review against Pro Git Ch. 7.7 Reset Demystified

`git reset` moves the branch pointer and then, depending on the mode, rewrites the
index and the working tree. Knowing which of the three you want is the whole skill.

- **`--soft` moves only the branch**: the changes stay staged, exactly as they were
  before the commit. This is how you re-commit with a better message or split a
  commit, without touching the files.
- **`--mixed` (the default)** moves the branch and clears the index: the changes are
  still in the working tree, unstaged. This is the "undo the commit, keep the work"
  mode, and the first half of splitting one.
- **`--hard` moves the branch, clears the index and overwrites the working tree.**
  Anything committed is recoverable from the reflog; anything never committed is
  gone. That is the only case where the warning is real.
- **`--keep` / `--merge` reset the branch but refuse (or carry) local changes** —
  the mode for "reset to origin, but do not destroy what I am in the middle of".
- **`HEAD~1` vs `<sha>` vs `--`** are different arguments: the first two move the
  branch, `git reset -- path` only unstages a path and moves nothing.
- **After a `--hard` mistake, `git reflog` is the recovery**: the commit is still
  there until the reflog expires.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
printf 'one\n' > a.txt && git add . && git commit -qm "add a"
printf 'two\n' > b.txt && git add . && git commit -qm "add b"
# --soft: the commit is gone, the change is staged, the file is untouched
git reset --soft HEAD~1
git diff --cached --name-only | grep -q "b.txt"
test -f b.txt
# --hard: this time the staged file goes with it
git reset --hard HEAD
if test -f b.txt; then exit 1; fi
