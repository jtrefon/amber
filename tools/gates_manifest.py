#!/usr/bin/env python3
"""Single inventory of the CI jobs and the local command for each gating one.

`ci.yml` (the job definitions), `gate_needs_check.py` (which skips are legal) and
the documentation used to carry the same job set three times. They drifted: a
gating job could be added to the workflow and never be required, because
`ci-gate` only judges what appears in its `needs` list. This module is the one
list; `tools/ci_manifest_check.py` compares it with the workflow, and
`gate_needs_check.py` derives its dimension table from it.

The field contract:

    dimension    always | cpp | web  -- what a diff must touch to affect the job
    events       workflow events the job applies to (an absent event means the
                 workflow scopes the job to the others)
    gating       whether a skip must be justified to `ci-gate`
    local        the command that reproduces it locally, or None
    local_reason why no local command exists (required when local is None)
    reason       why a non-gating job is excluded (required when gating is False)
    advisory     reports findings without blocking: the step must carry
                 `continue-on-error` so the check run concludes success

`tests/gates/test_ci_manifest.py` holds the invariant: every gating job is in
`ci-gate.needs`, in the skip policy, and has a local command or a stated reason.
"""

ALL_EVENTS = ("pull_request", "push", "merge_group", "workflow_dispatch")
PR_ONLY = ("pull_request",)
PUSH_OR_DISPATCH = ("push", "workflow_dispatch")

JOBS = {
    # The change detector. `ci-gate` checks it explicitly, so it is not
    # aggregated into itself.
    "pr-readiness": {
        "dimension": "always",
        "events": PR_ONLY,
        "gating": True,
        "local": None,
        "local_reason": "GitHub PR metadata (draft, body, labels); run "
                        "`python3 tools/pr_readiness.py --event <event.json>`",
    },
    "changes": {
        "dimension": "always",
        "events": ALL_EVENTS,
        "gating": False,
        "reason": "change detector; ci-gate checks it explicitly",
    },
    "dependency-review": {
        "dimension": "always",
        "events": PR_ONLY,
        "gating": True,
        "local": None,
        "local_reason": "GitHub dependency-review-action; no local equivalent",
    },
    # Full-tree clang-tidy on every PR. The incremental selector is still used
    # by `lint-changed` for the local loop, but it is not the CI gate: a feature
    # branch push starts no workflow (the push trigger is main/master only), so
    # a selector miss could only be discovered after the merge.
    "lint": {
        "dimension": "cpp",
        "events": PR_ONLY,
        "gating": True,
        "local": "make lint",
    },
    "test-presence": {
        "dimension": "cpp",
        "events": PR_ONLY,
        "gating": True,
        "local": "make test-presence",
    },
    "analyze": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make analyze",
    },
    "static-gates": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make static-gates",
    },
    "content-gates": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make content-gates",
    },
    "build-hygiene": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make check-build",
    },
    "complexity": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make complexity",
    },
    "duplicates": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make duplicates",
    },
    "obfuscation-guard": {
        "dimension": "always",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make obfuscation",
    },
    "format-check": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make format-check-changed",
    },
    "sanitizers": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": None,
        "local_reason": "needs an ASan/UBSan rebuild; use the parity container or CI",
    },
    "tsan": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": None,
        "local_reason": "needs a TSan rebuild; use the parity container or CI",
    },
    "secret-scan": {
        "dimension": "always",
        "events": ALL_EVENTS,
        "gating": True,
        "local": None,
        "local_reason": "gitleaks-action; install gitleaks locally to reproduce",
    },
    "bench-efficiency": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make bench-check",
    },
    "fuzz": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make fuzz-run",
    },
    "coverage": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make coverage",
    },
    "build-and-test": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "make test",
    },
    "build-and-test-macos": {
        "dimension": "cpp",
        "events": ALL_EVENTS,
        "gating": True,
        "local": None,
        "local_reason": "macOS runner only",
    },
    "website-build": {
        "dimension": "web",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "cd website && npm ci && npm run build",
    },
    "website-smoke": {
        "dimension": "web",
        "events": ALL_EVENTS,
        "gating": True,
        "local": "node website/scripts/smoke.mjs",
    },
    # Delivery jobs. They publish, so they are not part of the merge gate; they
    # depend on it (see ci.yml) rather than being aggregated by it.
    "cd": {
        "dimension": "always",
        "events": PUSH_OR_DISPATCH,
        "gating": False,
        "reason": "delivery job, main only",
    },
    "cd-macos": {
        "dimension": "always",
        "events": PUSH_OR_DISPATCH,
        "gating": False,
        "reason": "delivery job, main only",
    },
    "website-deploy": {
        "dimension": "web",
        "events": PUSH_OR_DISPATCH,
        "gating": False,
        "reason": "delivery job, main only",
    },
    "ci-gate": {
        "dimension": "always",
        "events": ALL_EVENTS,
        "gating": False,
        "reason": "aggregator; not aggregated into itself",
    },
    "commit-hygiene": {
        "dimension": "always",
        "events": PR_ONLY,
        "gating": False,
        "advisory": True,
        "reason": "advisory: the commit convention is not yet adopted consistently",
    },
}


def gating_jobs():
    """The jobs a skip has to justify to `ci-gate`."""
    return {job: row for job, row in JOBS.items() if row["gating"]}
