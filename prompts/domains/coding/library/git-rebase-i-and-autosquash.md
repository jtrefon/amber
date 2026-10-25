trigger: when a branch's history needs tidying before review, or a fixup commit is waiting to be folded
tags: #git
also: rebase, squash, fixup
weight: 65
verified: 2026-10
reference: https://git-scm.com/docs/git-rebase
sources: Pro Git 2ed, Ch. 7.6 Rewriting History (read in full, git-scm.com/book/en/v2)

`git rebase -i` is a script you edit before git runs it: each line is a commit and
the verb in front of it is what happens to that commit.

- **`commit --fixup=<sha>` marks a commit as belonging to an earlier one**; the
  next `rebase -i --autosquash` reorders it and changes its verb to `fixup`
  automatically. `--fixup=amend:<sha>` keeps the message (a `reword`-style fixup).
- **The verbs worth knowing**: `reword` (message only), `edit` (stop so you can
  amend content), `squash` (fold, keep both messages for editing), `fixup` (fold,
  discard this message), `drop`, and `exec <cmd>` — which is how a test run or a
  formatter becomes part of the history rewrite.
- **`edit` plus `reset HEAD^` splits a commit**: reset mixed, then stage and commit
  as many times as the split needs, then `rebase --continue`.
- **Never rewrite pushed history** on a branch others build on; on your own feature
  branch it is the normal way to turn twenty WIP commits into five reviewable ones.
- **A rebase is not a force-push yet**: run the tests before pushing, and prefer
  `--force-with-lease` so a teammate's push is not silently overwritten.
- **`rebase --abort`** returns to the pre-rebase state, and the reflog can recover
  afterwards anyway — so the cost of trying is a minute, not the branch.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
printf 'one\n' > a.txt && git add . && git commit -qm "add a"
printf 'two\n' > b.txt && git add . && git commit -qm "add b"
printf 'one fixed\n' > a.txt && git add . && git commit -qm "fixup! add a"
GIT_SEQUENCE_EDITOR=true git rebase -qi --autosquash --root
if git log --format=%s | grep -q "fixup!"; then exit 1; fi
git log --format=%s | grep -q "^add a$"
git show --stat --format=%s HEAD~1 | grep -q "a.txt"
