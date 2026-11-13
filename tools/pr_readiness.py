#!/usr/bin/env python3
"""PR readiness: a pull request is a request to review finished work.

The repo has one collaborator, and GitHub forbids self-approval, so a native
required-review count cannot be satisfied by the author. This check enforces the
mechanical half of the policy:

- a draft is not ready;
- the body carries verification evidence: a `## ...verification...` section with
  at least one populated line, not just the template bullets or checkboxes;
- a PR labelled `feature`, `bug`, `security` or `ci` also needs the
  `review:approved` label. Docs/dependency PRs and unlabelled PRs are gate-only.

The "one completed feature" rule and the quality of the evidence are review
judgements; this check only refuses the shapes that cannot be ready.

Usage:
  tools/pr_readiness.py --event "$GITHUB_EVENT_PATH"
  tools/pr_readiness.py --event tests/gates/fixtures/pr_event.json
"""

import argparse
import json
import os
import re
import sys

# The PR categories that carry a review promise. A docs/dependency PR is
# gate-only by design; an unlabelled PR is treated the same way so the label
# adds the requirement rather than assuming it.
REVIEW_LABELS = {"feature", "bug", "security", "ci"}
APPROVED_LABEL = "review:approved"

# The template's empty manual-E2E fields. They are labels, not evidence; a PR
# that leaves them blank has not recorded a run.
PLACEHOLDER = re.compile(r"^-\s+\*\*(Steps|Environment|Expected|Observed|Evidence):\*\*\s*$")


def sections(body):
    """{heading (lowercased): lines} for the `##` sections of a PR body."""
    out, current = {}, None
    for line in body.splitlines():
        if line.startswith("## "):
            current = line[3:].strip().lower()
            out.setdefault(current, [])
        elif current is not None:
            out[current].append(line)
    return out


def has_evidence(body):
    for title, lines in sections(body).items():
        if "verification" not in title:
            continue
        for raw in lines:
            line = raw.strip()
            if not line or line in ("-", "*"):
                continue
            if re.match(r"^- \[[ xX]\]", line):
                continue  # a checklist item, not a recorded result
            if re.match(r"^\d+\.$", line):
                continue  # an empty numbered step
            if PLACEHOLDER.match(line):
                continue
            return True
    return False


def is_dependency_pr(event):
    """Dependabot/dependency PRs are gate-only: they have no manual E2E to
    record and no feature to review, and the dependency gates are their test."""
    pull = event.get("pull_request") or {}
    author = (pull.get("user") or {}).get("login", "")
    return author == "dependabot[bot]" or "dependencies" in labels(event)


def labels(event):
    result = set()
    for label in (event.get("pull_request") or {}).get("labels") or []:
        if isinstance(label, dict) and label.get("name"):
            result.add(label["name"])
        elif isinstance(label, str):
            result.add(label)
    return result


def check(event):
    """(return code, message) for one GitHub event payload."""
    if not isinstance(event, dict) or "pull_request" not in event:
        return 0, "not a pull_request event; readiness is checked on PRs only"
    pull = event.get("pull_request") or {}
    if pull.get("draft"):
        return 1, "the PR is a draft; open it when the feature/bug is complete"
    if is_dependency_pr(event):
        return 0, "dependency PR; gate-only"
    if not has_evidence(pull.get("body") or ""):
        return 1, ("the PR body has no verification evidence; add a populated "
                   "'## Manual end-to-end verification' or '## Verification' "
                   "section with commands and observed results")
    present = labels(event)
    categories = present & REVIEW_LABELS
    if categories and APPROVED_LABEL not in present:
        return 1, ("this PR is labelled " + ", ".join(sorted(categories)) +
                   f"; add the {APPROVED_LABEL} label after review")
    return 0, "ready"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--event", default=None,
                    help="GitHub event JSON (default: $GITHUB_EVENT_PATH)")
    args = ap.parse_args()
    path = args.event or os.environ.get("GITHUB_EVENT_PATH", "")
    if not path:
        print("pr-readiness: FAILED CLOSED - no event JSON was supplied",
              file=sys.stderr)
        return 2
    try:
        with open(path, encoding="utf-8") as handle:
            event = json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"pr-readiness: FAILED CLOSED - cannot read {path}: {exc}",
              file=sys.stderr)
        return 2
    rc, message = check(event)
    print(f"pr-readiness: {message}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
