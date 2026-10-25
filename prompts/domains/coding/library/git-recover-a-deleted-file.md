trigger: when a file was deleted and someone needs it back, or a branch was reset away
tags: #git
also: deleted file, recover file
weight: 70
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://git-scm.com/docs/git-log#Documentation/git-log.txt---diff-filterACDMRTUXB82308203

Nothing committed is really lost; the question is which commit still has it.

- **Find the deletion**: `git log --diff-filter=D --format=%H -- path/to/file`
  lists the commits that deleted it — the newest is the one you want.
- **Read it from the commit before** the deletion:
  `git show <sha>^:path/to/file` prints the file as it was. Redirect it back into
  place: `git show <sha>^:path > path`.
- **A whole directory** works the same way (`-- path/dir/`), and `--all` searches
  every branch and tag, not just the current history.
- **For a reset or a bad rebase**, the reflog holds the commits: `git reflog`,
  then `git show HEAD@{3}` or `git branch rescue <sha>`. Reflog entries expire
  (90 days by default), so recover before housekeeping runs.
- **If it was never committed**, git cannot help — that is what a stash, a WIP
  commit or an editor's local history is for.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
mkdir -p docs && printf '# Old design\n' > docs/design.md
git add . && git commit -qm "add the design doc"
git rm -q docs/design.md && git commit -qm "drop the design doc"
sha=$(git log --diff-filter=D --format=%H -- docs/design.md | head -1)
test -n "$sha"
git show "$sha^:docs/design.md" | grep -q "Old design"
