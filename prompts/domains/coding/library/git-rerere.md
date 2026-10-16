trigger: when the same conflict keeps coming back after every rebase or merge
tags: #git
weight: 50
verified: 2026-10
reference: https://git-scm.com/docs/git-rerere
sources: model knowledge 2026-10; pending review against Pro Git Ch. 7.9 Rerere

Rerere (reuse recorded resolution) remembers how you resolved a conflict and
applies the same resolution the next time that conflict appears.

- **Turn it on once**: `git config --global rerere.enabled true`. There is no
  downside for a single developer and it costs nothing when it never fires.
- **What it does**: on a conflict, it records the pre-image and your resolution
  under `.git/rr-cache`; the next time the same pre-image appears — the same
  rebase replayed, the same branch merged again, a long-lived branch rebased
  daily — the resolution is applied for you.
- **`git rerere` with no arguments applies a recorded resolution** to the current
  conflict; `git rerere diff` shows what it would do; `git rerere status` lists
  what is recorded for the file.
- **`rerere.autoUpdate`** also stages the resolved files, which is convenient and
  slightly more surprising; the default (record and apply, leave staging to you)
  is the safer habit.
- **It is per-clone, not shared**: the cache lives in `.git`, so a teammate does
  not inherit your resolutions. The resolution is still a commit they will see.
- **A rebase that conflicts the same way every time is the use case**: rerere turns
  a ten-minute conflict resolution repeated weekly into a one-time decision.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
git config rerere.enabled true
printf 'base\n' > file.txt && git add . && git commit -qm "base"
git checkout -qb left && printf 'left\n' > file.txt && git commit -qam "left"
git checkout -q main && git checkout -qb right && printf 'right\n' > file.txt && git commit -qam "right"
# first time: resolve the conflict by hand, which is what rerere records
git merge left > /dev/null 2>&1 || true
printf 'left\n' > file.txt && git add file.txt && git commit -qm "merge left"
# replay the same conflict: this time the resolution is remembered
git reset -q --hard HEAD~1
git merge left > /dev/null 2>&1 || true
git rerere > /dev/null 2>&1 || true
if grep -q '<<<<<<<' file.txt; then exit 1; fi
grep -q '^left$' file.txt
