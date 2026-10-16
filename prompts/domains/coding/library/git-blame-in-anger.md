trigger: when blame points at a whitespace commit, or at a line that clearly came from somewhere else
tags: #git
weight: 55
verified: 2026-10
reference: https://git-scm.com/docs/git-blame

Blame is a starting point, not an answer: the goal is the *reason* a line looks the
way it does, and the defaults hide it.

- **`-M` finds lines moved within a file, `-C` lines copied from another file**
  modified in the same commit; without them a refactor inside one file shows
  every line as new. `-C -C` widens the search to the whole tree. A file *renamed* on its own is followed without either.
- **`-w` ignores whitespace-only changes**, which is what a reformat looks like.
- **`-L 40,60` blames a range** — the only way blame is readable on a real file.
- **`--ignore-rev <sha>`** skips one commit (a mass reformat) and
  `--ignore-revs-file .git-blame-ignore-revs` makes that permanent and shared.
  GitHub honours the same file.
- **Then read the commit, not the line**: `git show <sha>` gives the message, the
  diff and the issue reference that explain why.
- **When blame is a wall of one author and one date**, the file was imported or
  reformatted: find the import commit with `git log --follow` and read from there.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
mkdir -p src
printf 'def calculate():\n    return 42\n' > src/a.py
git add . && git commit -qm "add the calculation"
git mv src/a.py src/b.py && git add -A && git commit -qm "move the module"
# The file's own history starts at the move; the commit that *wrote* the lines
# is the oldest one touching the path they came from.
original=$(git log --format=%h -- src/a.py | tail -1)
# Blame prefixes a boundary commit (the file's first) with '^'; it still follows
# a rename on its own, which is what makes the attribution the original commit.
attributed=$(git blame -C -M src/b.py | head -1 | awk '{print $1}' | tr -d '^')
test "$original" = "$attributed"
