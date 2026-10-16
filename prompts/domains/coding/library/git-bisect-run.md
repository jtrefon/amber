trigger: when something broke between two known-good points and the cause is not obvious
tags: #git
weight: 65
verified: 2026-10
reference: https://git-scm.com/docs/git-bisect

Bisect turns "which commit broke this" into a binary search you can automate: you
supply a command that exits 0 for good and non-zero for bad, and git finds the
first bad commit.

- **Start with the bounds**: `git bisect start <bad> <good>`. A good tag or the
  release before the regression is the usual anchor.
- **`git bisect run <cmd>` automates the whole hunt**, including the checkout of
  each candidate. The command must be a script — anything that returns 125 is
  skipped (a build that cannot even configure is not the culprit).
- **`git bisect run` needs a reliable oracle**: a failing test, a
  reproduction script, a grep. A flaky oracle bisects to a random commit and
  wastes an hour.
- **A build step belongs inside the script**, not before: each candidate commit
  needs its own build.
- **`git bisect log > bisect.log` records the session**, and `git bisect replay`
  re-runs it after a mistake; `git bisect reset` returns the tree to where you
  started (and is required before switching branches).
- **Bisect answers "which", not "why"** — the first bad commit is where you now
  read the diff, with the pickaxe if the change is subtle.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
cat > check.sh <<'SRC'
#!/bin/sh
test ! -f broken.txt
SRC
chmod +x check.sh
git add . && git commit -qm "good: nothing broken"
good=$(git rev-parse HEAD)
for i in 1 2 3; do printf 'step %s\n' "$i" > "file$i.txt"; git add .; git commit -qm "step $i"; done
printf 'regression\n' > broken.txt && git add . && git commit -qm "break it"
for i in 4 5; do printf 'step %s\n' "$i" > "file$i.txt"; git add .; git commit -qm "step $i"; done
out=$(git bisect start HEAD "$good" > /dev/null 2>&1; git bisect run ./check.sh 2>&1 || true)
git bisect reset > /dev/null 2>&1 || true
printf '%s\n' "$out" | grep -q "break it"
