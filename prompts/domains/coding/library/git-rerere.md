trigger: when the same conflict keeps coming back after every rebase or merge
tags: #git
weight: 50
verified: 2026-10
reference: https://git-scm.com/docs/git-rerere
sources: Pro Git 2ed, Ch. 7.9 Rerere (read in full, git-scm.com/book/en/v2)

Rerere — "reuse recorded resolution" — remembers how you resolved a hunk conflict
and applies the same resolution the next time that exact conflict appears.

- **Turn it on once**: `git config --global rerere.enabled true` (or create
  `.git/rr-cache`, which is the same thing less legibly).
- **Git tells you what it is doing**: on a conflict, `Recorded preimage for FILE`;
  when you resolve and commit, `Recorded resolution for FILE`; and later, instead
  of a conflict, `Resolved FILE using previous resolution`.
- **The interesting tactic is *backing out* the merge.** Attempt the merge into a
  long-lived branch, resolve the conflicts, then rewind the merge — the
  resolutions stay recorded, so the final merge is nearly free. The same trick
  keeps a topic branch rebased without resolving the identical conflicts every
  week.
- **Inspect before trusting it**: `git rerere status` lists what is recorded for
  the file, and `git rerere diff` shows what the recorded resolution will be.
- **`git rerere` with no arguments re-applies the recorded resolution** to the
  current conflict; `git checkout --conflict=merge FILE` puts the conflict markers
  back, and `git checkout --conflict=diff3` shows the common ancestor too.
- **The cache is per-clone** (`.git/rr-cache`), so resolutions are not shared with
  teammates; and a resolution is still a commit someone will review — rerere saves
  the typing, not the decision.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
git config rerere.enabled true
printf 'base\n' > file.txt && git add . && git commit -qm "base"
git checkout -qb left && printf 'left\n' > file.txt && git commit -qam "left"
git checkout -q main && git checkout -qb right && printf 'right\n' > file.txt && git commit -qam "right"
# the first conflict is resolved by hand, which is what rerere records
git merge left > /dev/null 2>&1 || true
git rerere status | grep -q file.txt
printf 'left\n' > file.txt && git add file.txt && git commit -qm "merge left"
# back the merge out, then replay: this time the resolution is remembered
git reset -q --hard HEAD~1
git merge left > /dev/null 2>&1 || true
git rerere > /dev/null 2>&1 || true
if grep -q '<<<<<<<' file.txt; then exit 1; fi
grep -q '^left$' file.txt
