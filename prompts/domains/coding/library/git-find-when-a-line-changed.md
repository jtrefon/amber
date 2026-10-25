trigger: when you need to know when a value, a string or a line appeared or changed
tags: #git
also: blame, git log -L
weight: 65
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://git-scm.com/docs/git-log#Documentation/git-log.txt--S

`git log -S` is the pickaxe: it finds commits where the *number of occurrences* of
a string changed. `-G` matches the diff instead — use it when the line changed
rather than appeared.

- **`git log -S'the string' -- path`** answers "when did this appear or
  disappear". It is the fastest way to find the commit that introduced a
  constant, a workaround or a mistake.
- **`-G<regex>` shows commits whose diff touches a matching line** — the right
  tool when the value changed but the string stayed (`-G'rate_limit'`).
- **`-p` to see the change**, `--follow` to keep tracking a file across renames,
  `--all` to search branches nobody has checked out, `--reverse` for the first hit.
- **`-S` accepts a regex only with `-G`**; plain `-S` is a literal count, which is
  why `-S'foo'` can miss a line that changed from `foo(1)` to `foo(2)`.
- **Bisect is the other half**: when you know it broke but not what changed,
  `git bisect` narrows by testing, and the pickaxe narrows by reading.

## Run it

```run
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.com GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.com
git init -q -b main repo && cd repo
printf 'rate_limit = 10\n' > config.py
git add config.py && git commit -qm "add config"
printf 'rate_limit = 25\n' > config.py
git add config.py && git commit -qm "raise the rate limit"
git log --oneline -S'rate_limit = 25' -- config.py | grep -q "raise the rate limit"
if git log --oneline -S'rate_limit = 99' -- config.py | grep -q .; then exit 1; fi
