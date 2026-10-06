# The broken build on `main`, and a merge-process gap

Status: **proposal, awaiting sign-off.** One gate is currently red on `main` and needs a
decision before anything else here.

## First: no, I did not force-push and I did not push directly to `main`

Measured, because the accusation is specific enough to check:

```
main is linear: 4 merge commits total, all squash (single parent)
3685b7a (#221)  1 parent -> ac6e234
392691e (#222)  1 parent -> 3685b7a
5a5695a (#223)  1 parent -> 392691e

enforce_admins: true    required checks: 1 (ci-gate)    strict: true
force_push: false       deletions: false
```

All three of my commits are squash-merged PRs with `(#NNN)` suffixes. The 4 real merge
commits in history are all from **August** (`344deea`, `0f3971b`, `2f2ab71`, `7d0b451`) —
before branch protection existed. The `git push -f` I used earlier today was against
`docs/outstanding-items-proposal`, a throwaway feature branch, to drop a commit before
opening its PR. Never `main`.

## But you found something real, and it is worse than a force-push

**`main` is red right now.** `website-build` has been failing since `392691e` (#222), and
`ci-gate` fails with it:

```
4b9477a  website-build: success
ac6e234  website-build: success     (#220)
3685b7a  website-build: success     (#221)
392691e  website-build: failure     (#222)   <- red since here
5a5695a  website-build: failure     (#223, current tip)
```

The cause is a **new advisory**, not my diff. #222 changed only `AGENTS.md` and two
markdown files:

```
audit: blocking advisories
  GHSA-68fv-2mgg-jv7q (high) in source-map-js: event-loop DoS through indexed
  source-map section offsets
```

`source-map-js@1.2.1`, `CVE-2026-93749`, **no patched version exists** — identical
situation to the `http-cache-semantics` advisory already in
`website/audit-exceptions.json`. It is transitive via `postcss`, `css-tree` and
`magicast`, and no source file in `website/` references it.

**So: an unvetted external advisory landed, and the gate correctly went red. That is the
gate working.** The failure is in how I *handled* it, which is the next section.

## How I merged with a red gate — the actual process failure

This is the part worth writing down, because I did not notice it and you did.

On #222 I read my check summary as:

```
7 pass, 18 skipping
```

and merged. `website-build` was **SKIPPED, not passed**. `ci-gate` reported SUCCESS
because `tools/gate_needs_check.py` correctly judged that a docs-only diff cannot affect
the website dimension. Every individual part was right. The **summary I read was wrong**:
"18 skipping" read as "18 irrelevant", when for this gate set it meant 18 unmeasured — and
the one that broke landed on `main` two minutes later.

The deeper issue: **a gate that only runs on paths it recognises cannot protect a
mainline from an advisory published between runs.** The path filter was right; my
reading of it was not. And `ci-gate` returning SUCCESS while a sibling gating job was
skipped is a genuine design smell worth fixing — it is the same class of problem as the
skip-vs-pass bug you already had me fix in #214, and I introduced a new instance of it
in how I consumed the output.

I should also be direct about the earlier claim: I told you "`make check` green, all
green". That was true locally and true for the C++ gates. It was not true of `main`, and
I did not check `main`.

## What needs fixing

### 1. `main` is red and needs a decision now

`source-map-js` has no patch, so `npm audit fix` cannot fix it. The existing precedent
is `website/audit-exceptions.json`: an entry with a **reason and an expiry**, after which
the gate re-arms itself.

Proposed entry, matching the existing one:

```json
{
  "id": "GHSA-68fv-2mgg-jv7q",
  "package": "source-map-js",
  "severity": "high",
  "reason": "Transitive via postcss/css-tree/magicast; no patched version exists for CVE-2026-93749. Event-loop DoS requires parsing an attacker-supplied indexed source map, which a static Astro build never does: no source file in website/ references source-map-js. Re-check when postcss drops it.",
  "expires": "2027-01-15"
}
```

**I am not going to apply this without your sign-off.** It is a suppression, and my own
sealed-pipeline proposal says a suppressed high-severity advisory is worse than a visible
one. The alternatives are to accept `main` stays red, or to downgrade `astro` 4→2
(masive, and still not a fix). Your call — see the question at the end.

### 2. Record the merge rule in `AGENTS.md`, as you asked

New subsection under Branching strategy, stating plainly:

- `main` is never pushed to directly, by anyone, including admins and including me.
- No force-push to `main`, ever. The escape hatch
  (`gh api -X DELETE .../protection`) is for a declared emergency, and re-applying it is
  part of that emergency, not an afterthought.
- No merge of a PR whose `ci-gate` is red **or skipped for a reason that leaves the
  change unmeasured**.
- Squash-merge only, to keep `main` linear.

The third bullet is the one that would have caught today's mistake.

### 3. Close the skip-vs-pass hole in how ci-gate is read

Not by changing the gate — the path filter is correct and I am not touching it — but by
making the failure mode unmissable:

- `gate_report.py` prints the **count of skipped gating jobs by name**, not just verdicts.
- If a `ci-gate` verdict is SUCCESS with any of its 18 dependencies skipped, the summary
  line reads `PASS (13 run, 5 skipped: website-*, dependency-review)` instead of a bare
  `PASS`.
- This belongs in the gate's own `tests/gates/`, per the rule that a gate change ships
  with its tests.

The principle, which is the same one already written into `AGENTS.md` about this repo:
**a skip is not a pass.** I violated it as the consumer of the output, so the fix has to
make the violation loud.

### 4. A dated-advisory check that does not need a code change

Advisories arrive on a schedule, not on a commit schedule. `main` should not depend on
the next commit to notice one. Proposal: a scheduled (not push-triggered) `advisory`
job running `npm audit`, wired into `always`, that fails when a **new unexcepted**
advisory appears — so the repo learns about it on a predictable cadence instead of the
first time someone happens to push.

This is the fix for the class of problem, not the instance.

## Order

1. Decide on `main` (item 1) — it is red now, and everything else is easier on green.
2. `AGENTS.md` merge rule (item 2) — your explicit ask, smallest diff, do it first.
3. Gate-report skip visibility + its tests (item 3).
4. Scheduled advisory job (item 4).

## What I am not proposing

Not rewriting history. `main` has two red commits that are documentation-only and
correct; rewriting them to hide a red build is worse than an honest red build, and branch
protection forbids it anyway.

Not auto-expiring into silence. The exception's `expires` is what stops a suppression
becoming permanent, and that mechanism already exists and works.