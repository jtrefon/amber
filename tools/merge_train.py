#!/usr/bin/env python3
"""Merge train: land one labelled PR per run, then keep the rest fresh.

GitHub's native merge queue is not available on this repository (the rulesets API
rejects a `merge_queue` rule, and there is no REST or GraphQL field that enables it),
so a green PR cannot merge once `main` has moved -- `strict` required checks make it
"out of date", and PRs pile up waiting for somebody to press "Update branch".

This is the queue without the queue. Opting in is the label `merge-train`: the train
never touches a PR nobody asked it to land. Each run:

  1. takes the lowest-numbered labelled PR whose `mergeStateStatus` is CLEAN and merges
     it (squash) -- exactly one, because a second merge would be validated against a
     `main` that no longer exists;
  2. updates the branch of every other labelled PR that is BEHIND, so its checks re-run
     against the new `main` and it is CLEAN on a later run.

DIRTY (a real conflict), BLOCKED (checks still running, or a gate failed), DRAFT and
unlabelled PRs are left alone: a train cannot resolve a conflict, and silently merging
past a red gate is the failure the gates exist to prevent.

`--selftest` checks the selection rules on fixture PRs and never talks to GitHub.
"""

import json
import sys

LABEL = "merge-train"

# The states where acting is safe. Anything else is left for a human: DIRTY needs a
# conflict resolved, BLOCKED means a gate has not passed, DRAFT is not offered.
MERGEABLE, NEEDS_UPDATE = "CLEAN", "BEHIND"


def plan(prs):
    """(number to merge or None, [numbers to update]) for one run.

    `prs` is a list of dicts with `number`, `isDraft`, `labels`, `mergeable`,
    `mergeStateStatus`, in the order they should be considered (oldest first).
    """
    eligible = [p for p in prs
                if not p.get("isDraft")
                and LABEL in {label.get("name") for label in p.get("labels", [])}]
    merge, update = None, []
    for pr in eligible:
        state = pr.get("mergeStateStatus")
        if state == MERGEABLE and merge is None:
            merge = pr["number"]
            continue
        # One merge per run: anything after it was validated against the old main.
        if state == NEEDS_UPDATE:
            update.append(pr["number"])
        elif state == MERGEABLE:
            update.append(pr["number"])
    return merge, update


def _selftest():
    def pr(number, state, label=True, draft=False):
        labels = [{"name": LABEL}] if label else []
        return {"number": number, "mergeStateStatus": state, "isDraft": draft, "labels": labels}

    cases = [
        ("nothing queued", [], (None, [])),
        ("one clean PR merges", [pr(10, "CLEAN")], (10, [])),
        ("an unlabelled PR is never touched", [pr(10, "CLEAN", label=False)], (None, [])),
        ("a draft is not offered", [pr(10, "CLEAN", draft=True)], (None, [])),
        ("BLOCKED is left alone", [pr(10, "BLOCKED")], (None, [])),
        ("DIRTY is left alone", [pr(10, "DIRTY")], (None, [])),
        ("only one merge per run, oldest first",
         [pr(12, "CLEAN"), pr(10, "CLEAN")], (12, [10])),
        ("a BEHIND PR is updated, not merged", [pr(10, "BEHIND")], (None, [10])),
        ("strictness does not update an unlabelled PR", [pr(10, "BEHIND", label=False)],
         (None, [])),
        ("merge and update together",
         [pr(10, "CLEAN"), pr(11, "BEHIND"), pr(12, "BLOCKED")], (10, [11])),
    ]
    failures = 0
    for desc, prs, want in cases:
        got = plan(prs)
        if got != want:
            failures += 1
            print(f"  FAIL {desc}: want {want}, got {got}", file=sys.stderr)
        else:
            print(f"  ok   {desc}")
    if failures:
        print(f"merge_train: {failures} self-test case(s) failed", file=sys.stderr)
        return 1
    print(f"merge_train: {len(cases)} self-test cases passed")
    return 0


def main(argv):
    if "--selftest" in argv:
        return _selftest()
    try:
        prs = json.load(sys.stdin)
    except ValueError as exc:
        print(f"merge_train: FAILED CLOSED - could not read the PR list: {exc}", file=sys.stderr)
        return 2
    if not isinstance(prs, list):
        print("merge_train: FAILED CLOSED - the PR list is not a list", file=sys.stderr)
        return 2
    merge, update = plan(sorted(prs, key=lambda p: p.get("number", 0)))
    # GITHUB_OUTPUT is how the workflow consumes this; plain stdout keeps it usable by
    # hand, which is how the selection can be inspected without acting.
    lines = [f"merge={'' if merge is None else merge}",
             f"update={','.join(str(n) for n in update)}"]
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
