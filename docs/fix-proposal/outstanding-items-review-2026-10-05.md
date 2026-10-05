# Outstanding items — measured assessment and proposal

Status: **proposal, awaiting sign-off.** No deletions in this change; two of the three
recommendations are irreversible if done wrongly.

Every number here was measured, not recalled. Two of my earlier statements about this
backlog turned out to be wrong once measured, and both corrections are recorded below
because they change what the right action is.

## Summary

| # | item | measured state | proposal | risk |
|---|---|---|---|---|
| 1 | 43 branches with "unique" commits | **historical**; 72% of their lines already in `main`, top branches 95–100% | archive, do not delete | low |
| 2 | `stash@{0}` TUI refactor | **functionally superseded** by merged work | drop, keep the safety branch | low |
| 3 | `stash@{1}` website/docs | genuinely unlanded (5% in `main`) | **needs your decision** | — |
| 4 | `stash@{2}` commandcode | **empty** — two parents, zero diff | drop | none |
| 5 | `http-cache-semantics` high advisory | no patched version exists | no action possible; keep documented | none |
| 6 | `md4c` upstream revision | checksums enforced, revision unrecorded | one network fetch | low |

## Two corrections to what I told you earlier

**1. "45 branches are provably redundant, 43 have unique commits" was the wrong
frame.** `git cherry` compares patch-ids, and squash-merging destroys patch-id
correspondence. So "unique commit" does not mean "unmerged work".

I checked rather than argued: `tui/god-code-decomposition` reports 31 unique commits,
but its artefacts — `docs/spec/tui/architecture.md`, `tui/help_page.cpp`,
`tui/completion_context.cpp`, `tui/input_line_layout.cpp` — are all in `main`, because
the work landed as squash-merge #153.

Measuring properly (for each branch, take its unique commits' substantive added lines and
ask whether they are present in `main`):

```
OVERALL: 8082 / 11164 = 72% of unique added lines already present in main
  100%  275/276  refactor/core-config-decomposition
  100%  398/400  feature/session-brief
   99%  111/112  refactor/tool-call-parser
   98%  394/400  refactor/tui-input-loop-hexagonal
   95%  381/400  fix/security-hardening-v1
```

The residual is dominated by **doc text that was later rewritten**, not code that never
landed — the largest "missing" cluster is the `cpp_source.py` lexer docstring from
`ci/nesting-and-debt-gates`, which shipped in #203 with different wording.

**Conclusion: these 43 are history, not pending work.** They are not "43 things to
review"; they are the pre-squash-merge shape of work that is already in `main`.

**2. `stash@{0}` measured 0% of its lines present in `main`, which is also the wrong
reading.** The missing lines are:

```
+        OpenPanels,         // Alt+0 / ESC+0
+    int arg = -1;           // window index
+    if (key.key == 0xB0) return {KeyAction::OpenPanels, -1};
```

All three are the *same intent* as what is now in `main`, with different comments and a
different implementation of the same dispatch. `main` produces `OpenPanels` for `0xB0` —
`keyaction_every_variant_is_reachable_from_a_key` asserts it. So the measurement says
"different text", not "missing work".

All three of that stash's goals are resolved: the vocabulary is total, Alt+0 is a typed
intent, and the input-loop extraction was superseded by a decomposition that landed long
ago.

## Proposal

### 1. The 43 branches — archive, do not delete

Deleting is wrong even though the work landed: squash-merge means the branch commits are
the only place some intermediate states exist, and "72%" is not "100%". Irreversible
deletion on a 72% measurement is exactly the mistake class this repo audits for.

Instead:

- rename all 43 under a single `archive/` prefix, so `git branch` becomes readable
  again and nothing is destroyed;
- export them to a bundle at `archive/branches-2026-10-05.bundle` so they survive even a
  local clone being discarded;
- add one line to `.gitignore`-adjacent docs noting the bundle path.

Result: `git branch` goes from 48 lines to ~6, and nothing is unrecoverable. If you would
rather not carry them at all, the bundle is the checkpoint that makes deletion safe.

### 2. `stash@{0}` — drop, keep `wip/stash0-tui-refactor`

Functionally superseded (above). The safety branch already holds the commit, so dropping
the stash entry loses nothing:

```sh
git stash drop stash@{2}   # after the others, so indices do not shift
```

Proposed order: drop 2 first, then 0, leaving `stash@{0}` as the website WIP so its
index stays stable at 0.

### 3. `stash@{1}` — your decision, not mine

This one is **genuinely unlanded**: 5% of its lines exist in `main`, and the content is
website documentation that was never merged. It is also 103 files against a base 102
commits old, including *both-added* conflicts on `.github/ISSUE_TEMPLATE/`.

I am not touching it. It is your WIP, it is not stale by any measure, and the useful
question is one only you can answer: **is the website work still wanted?** If yes, it
wants a rebase onto current `main` as its own change, and the docs half will conflict
with every `AGENTS.md`/`README` edit since. If the website plan changed, the honest move
is to drop it and stop carrying a 103-file rebase that will never be taken.

Either way, converting it from a stash to a branch is worth doing on its own merits —
stashes are reflog-backed and easy to lose; branches are not.

### 4. `stash@{2}` — drop

Two parents, zero diff. It is an empty entry; there is nothing in it.

### 5. `http-cache-semantics` — no action is possible

High severity, and no patched version exists, so it cannot be closed by an upgrade or an
`overrides` pin. It is a transitive dependency of `astro`, and the affected path is
Astro's dev/preview HTTP cache, which is not exposed to multiple users. Already recorded
in `AGENTS.md` with the trigger for revisiting. **Recommend leaving it** and declining to
suppress it, because a suppressed high-severity alert is worse than a visible one.

### 6. `md4c` revision — one fetch, and it needs network access

The sha256 pair is now enforced by `build-hygiene` P11, so a silent edit to the vendored
parser cannot pass. What is missing is *provenance*: the README says "upstream `master`
snapshot, fetched 2026-07-18", which names no commit.

This cannot be reconstructed offline — a hash of the vendored files does not map back to
an upstream commit. It needs one `git clone` + `git log` against upstream to find the
commit whose `md4c.c`/`md4c.h` match our checksums, then record it. I have no network
access to upstream from here, so this is the one item I cannot complete.

## What I would do, in order

1. Archive the 43 branches + write the bundle. *(no sign-off needed beyond this doc; nothing destroyed)*
2. Drop `stash@{2}` and `stash@{0}`. *(irreversible, hence this proposal)*
3. Convert `stash@{1}` to a branch so it stops being a stash, and leave its content alone
   pending your answer on whether the website work is still wanted.
4. `md4c` provenance, when a network is available.

## The one thing worth deciding now

**Is the website work still wanted?** It is the only item on this list with real content
at stake, and items 1, 2 and 4 are bookkeeping that can proceed either way.
