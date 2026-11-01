#!/usr/bin/env python3
"""Decide whether a CI run's gating jobs may be called green.

`ci-gate` in .github/workflows/ci.yml collects every heavyweight job and must fail when
one of them did not pass. The awkward part is *skips*: a documentation-only PR
legitimately skips the C++ suite, so "skipped is a pass" was the rule for a while. That
rule is a bypass -- `if: false` on a gate, or a path filter that misclassified a
directory, turns the pipeline green with the gate never having run.

So a skip is legitimate only for the dimension `changes` says the diff cannot affect:

    always   examines the whole tree, so it must run
    cpp      the C++ suite
    web      the website build and smoke test

The two are independent: a C++-only PR legitimately skips the website jobs. Judging
them with one flag blocks every non-website PR, which is what the first attempt did.

Reads the `needs` JSON on stdin; writes the offending jobs to stdout and exits 1 if the
run may not be called green. `--selftest` runs the table above.
"""

import json
import os
import sys

import gates_manifest

ALWAYS = "always"
PULL_REQUEST_ONLY = "pull_request"
# Every event this workflow can run for. A job is only "not applicable" when the
# event is missing from its own tuple, so an event absent from here would make
# every skipped job look legitimately skipped -- the fail-open this table exists
# to prevent. `merge_group` is the merge queue's synthetic ref: the queue runs
# the whole suite there, so only the pull_request-only jobs may skip.
# `workflow_dispatch` is a first-class event here because the merge train has to use
# it: a branch update made with GITHUB_TOKEN creates no `synchronize` event, so the
# refreshed head would never get a `ci-gate` and the PR would stay BLOCKED forever.
# A dispatch run is held to the same jobs a push is.
ALL_EVENTS = gates_manifest.ALL_EVENTS

# Dimensions and event scopes come from the one inventory. Keeping a second copy
# here is how a new gating job could be added to the workflow and never judged.
_GATING = gates_manifest.gating_jobs()
DIMENSIONS = {job: row["dimension"] for job, row in _GATING.items()}
EVENTS = {job: row["events"] for job, row in _GATING.items()
          if row["events"] != ALL_EVENTS}

# The self-test builds its fixtures from THIS list, never from DIMENSIONS. Deriving them
# from the table under test makes the test agree with whatever the table says: a
# mis-mapped entry changes the fixture instead of failing, which is the "gate that
# measures nothing" shape. An independent list means a bad mapping changes behaviour.
ALL_JOBS = (
    "dependency-review",
    "obfuscation-guard",
    "secret-scan",
    "lint",
    "build-and-test",
    "build-and-test-macos",
    "analyze",
    "check",
    "complexity",
    "duplicates",
    "format-check",
    "sanitizers",
    "tsan",
    "fuzz",
    "coverage",
    "website-build",
    "website-smoke",
)
# The fixture partitions ALL_JOBS so a job cannot be silently unclassified. `lint`
# is both cpp and pull_request-only; it lives in PR_ONLY_JOBS, not CPP_JOBS, so
# the four tuples are disjoint.
CPP_JOBS = ("build-and-test", "build-and-test-macos", "analyze", "check",
            "complexity", "duplicates", "format-check", "sanitizers", "tsan",
            "fuzz", "coverage")
WEB_JOBS = ("website-build", "website-smoke")
ALWAYS_JOBS = ("obfuscation-guard", "secret-scan")
PR_ONLY_JOBS = ("dependency-review", "lint")

NOT_APPLICABLE = {"skipped", "neutral"}


def offenders(needs, cpp_changed, web_changed, event="pull_request"):
    """Jobs whose result means this run may not be called green."""
    bad = []
    changes = needs.get("changes", {}).get("result")
    if changes != "success":
        bad.append(f"changes [{ALWAYS}]: {changes}")

    def not_applicable(job, dimension):
        """Whether this job was skipped for a reason that is legitimate here.

        Two independent reasons, and conflating them is what made every push to main red:
        the diff cannot affect the job's dimension, or the workflow scopes the job to a
        different event than the one running. `lint` is pull_request-only because the
        full lint runs there; a feature-branch push starts no workflow at all.
        """
        if EVENTS.get(job, ALL_EVENTS) and event not in EVENTS.get(job, ALL_EVENTS):
            return True
        return (dimension == "cpp" and not cpp_changed) or (dimension == "web" and not web_changed)

    for job, info in sorted(needs.items()):
        if job == "changes":
            continue
        result = info.get("result")
        dimension = DIMENSIONS.get(job, ALWAYS)
        if result == "success":
            continue
        if result in NOT_APPLICABLE:
            if not not_applicable(job, dimension):
                bad.append(f"{job} [{dimension}]: {result} but this diff can affect it")
            continue
        # A failure, a cancellation, or a result this gate has never heard of. An unknown
        # result is treated as a failure on purpose: a new GitHub outcome must not be a
        # quiet pass.
        bad.append(f"{job} [{dimension}]: {result}")
    return bad


def check(needs, cpp_changed, web_changed, event="pull_request"):
    bad = offenders(needs, cpp_changed, web_changed, event)
    if bad:
        print("gating jobs did not pass:")
        for item in bad:
            print(f"  {item}")
        return 1
    return 0


def _check_fixture_coverage():
    """Every job the policy knows about must appear in the self-test fixture list.

    A job missing from ALL_JOBS is silently untested: its skip cases cannot be generated,
    so the policy stops covering it without any test failing. That is exactly what
    happened when `secret-scan` was added to DIMENSIONS and not to the fixture list -- the
    test failed, which is why this check exists to make the failure legible.

    The fixture is also checked against the manifest-derived tables: the four
    explicit tuples must partition the gating jobs, and each tuple must agree
    with the dimension/event the inventory assigns. Without that, a mis-mapped
    job moves the fixture instead of failing.
    """
    errors = []
    missing = sorted(set(DIMENSIONS) - set(ALL_JOBS))
    if missing:
        errors.append(f"gating jobs missing from the self-test fixture: {missing}")
    extra = sorted(set(ALL_JOBS) - set(DIMENSIONS))
    if extra:
        errors.append(f"self-test fixture jobs that are not gating jobs: {extra}")
    if len(ALL_JOBS) != len(set(ALL_JOBS)):
        errors.append(f"self-test fixture has duplicate jobs: {ALL_JOBS}")

    declared = set(CPP_JOBS) | set(WEB_JOBS) | set(ALWAYS_JOBS) | set(PR_ONLY_JOBS)
    unclassified = sorted(set(ALL_JOBS) - declared)
    unknown = sorted(declared - set(ALL_JOBS))
    if unclassified:
        errors.append(f"gating jobs in no fixture tuple: {unclassified}")
    if unknown:
        errors.append(f"fixture-tuple jobs that are not gating jobs: {unknown}")

    expected = (
        (CPP_JOBS, "cpp", None),
        (WEB_JOBS, "web", None),
        (ALWAYS_JOBS, "always", None),
        (PR_ONLY_JOBS, None, PULL_REQUEST_ONLY),
    )
    for jobs, dimension, event in expected:
        for job in jobs:
            if dimension is not None and DIMENSIONS.get(job) != dimension:
                errors.append(f"{job!r} is in a {dimension} tuple but the manifest "
                              f"says {DIMENSIONS.get(job)!r}")
            if event is not None and EVENTS.get(job, ALL_EVENTS) != (event,):
                errors.append(f"{job!r} is in a {event} tuple but the manifest says "
                              f"{EVENTS.get(job, ALL_EVENTS)!r}")

    for error in errors:
        print(f"gate_needs_check: {error}", file=sys.stderr)
    return 1 if errors else 0


def _needs(running, changes="success"):
    """A needs map where exactly the named jobs ran and every other known job skipped."""
    out = {"changes": {"result": changes}}
    for name in ALL_JOBS:
        out[name] = {"result": "success" if name in running else "skipped"}
    return out


ALL_RUNNING = set(ALL_JOBS)
WITHOUT = {name: set(ALL_JOBS) - {name} for name in ALL_JOBS}


def selftest():
    PR, PUSH = "pull_request", "push"
    cases = [
        # (description, needs, cpp, web, want_failure, event)
        ("C++ PR: everything runs", _needs(ALL_RUNNING), True, True, False, PR),
        ("C++-only PR: web skipped", _needs(set(ALL_JOBS) - set(WEB_JOBS)), True, False, False, PR),
        ("web-only PR: cpp skipped", _needs(set(ALL_JOBS) - set(CPP_JOBS)), False, True, False, PR),
        ("docs-only PR: cpp and web skipped",
         _needs(set(ALWAYS_JOBS) | set(PR_ONLY_JOBS)), False, False, False, PR),
        ("changes itself fails", _needs(ALL_RUNNING, changes="failure"), True, True, True, PR),
        ("a gate is cancelled", _needs(ALL_RUNNING) | {"tsan": {"result": "cancelled"}}, True, True, True, PR),
        ("a gate reports an unknown result", _needs(ALL_RUNNING) | {"lint": {"result": "weird"}}, True, True, True, PR),
        ("BYPASS: unlisted gating job skipped",
         _needs(ALL_RUNNING) | {"brand-new-gate": {"result": "skipped"}}, True, True, True, PR),
        ("unlisted gating job that ran is fine",
         _needs(ALL_RUNNING) | {"brand-new-gate": {"result": "success"}}, True, True, False, PR),
    ]
    # The bypass test: skipping a job the diff CAN affect must block.
    for name in ALL_JOBS:
        cases.append((f"BYPASS: {name} skipped on a C++ PR",
                      _needs(set(ALL_JOBS) - {name}), True, True, True, PR))
    # Legitimate skips: the diff cannot affect the dimension, or the job is PR-only.
    for name in WEB_JOBS:
        cases.append((f"{name} skipped on a non-website PR",
                      _needs(set(ALL_JOBS) - {name}), True, False, False, PR))
    for name in CPP_JOBS:
        cases.append((f"{name} skipped on a docs-only PR",
                      _needs(set(ALL_JOBS) - {name}), False, False, False, PR))
    # An "always" job scans the whole tree, so no diff makes it skippable.
    for name in ALWAYS_JOBS:
        cases.append((f"{name} skipped even on a docs-only PR",
                      _needs(set(ALL_JOBS) - {name}), False, False, True, PR))
    # dependency-review is "always" too, but it is also pull_request-only, so it
    # is not in ALWAYS_JOBS (that tuple is the disjoint fixture). A docs-only PR
    # still has to run it.
    cases.append(("BYPASS: dependency-review skipped even on a docs-only PR",
                  _needs(set(ALL_JOBS) - {"dependency-review"}), False, False, True, PR))
    # PR-only jobs skipped on a push: legitimate, and the reason main used to be red.
    for name, events in sorted(EVENTS.items()):
        cases.append((f"{name} skipped on a push (it is {events[0]}-only)",
                      _needs(set(ALL_JOBS) - {name}), True, True, False, PUSH))
        cases.append((f"BYPASS: {name} skipped on a pull request",
                      _needs(set(ALL_JOBS) - {name}), True, True, True, PR))
    # A push must still be held to every job that does apply to it.
    for name in CPP_JOBS + ALWAYS_JOBS + WEB_JOBS:
        cases.append((f"BYPASS: {name} skipped on a push",
                      _needs(set(ALL_JOBS) - {name}), True, True, True, PUSH))
    # The merge queue runs the whole suite on the queued merge commit, so it is
    # held to the same jobs a push is -- and the pull_request-only jobs skip
    # there for the same reason they skip on a push.
    MQ = "merge_group"
    cases.append(("merge queue: everything runs", _needs(ALL_RUNNING), True, True, False, MQ))
    for name, events in sorted(EVENTS.items()):
        cases.append((f"{name} skipped on a merge queue run (it is {events[0]}-only)",
                      _needs(set(ALL_JOBS) - {name}), True, True, False, MQ))
    for name in CPP_JOBS + ALWAYS_JOBS + WEB_JOBS:
        cases.append((f"BYPASS: {name} skipped on a merge queue run",
                      _needs(set(ALL_JOBS) - {name}), True, True, True, MQ))
    # A merge-train branch refresh is a GITHUB_TOKEN push: it starts no CI. The train
    # dispatches ci.yml on the branch instead, and that run must be held to the push
    # job set -- otherwise the refresh route becomes a way to skip a gate.
    WD = "workflow_dispatch"
    cases.append(("dispatch: everything runs", _needs(ALL_RUNNING), True, True, False, WD))
    for name, events in sorted(EVENTS.items()):
        cases.append((f"{name} skipped on a dispatch run (it is {events[0]}-only)",
                      _needs(set(ALL_JOBS) - {name}), True, True, False, WD))
    for name in CPP_JOBS + ALWAYS_JOBS + WEB_JOBS:
        cases.append((f"BYPASS: {name} skipped on a dispatch run",
                      _needs(set(ALL_JOBS) - {name}), True, True, True, WD))
    failures = _check_fixture_coverage()
    for desc, needs, cpp, web, want_fail, event in cases:
        bad = offenders(needs, cpp, web, event)
        got_fail = bool(bad)
        if got_fail != want_fail:
            failures += 1
            verdict = "should BLOCK" if want_fail else "should pass"
            print(f"  FAIL {desc}: {verdict}, got {bad or 'pass'}", file=sys.stderr)
        else:
            note = f" ({bad[0]})" if bad else ""
            print(f"  ok   {desc}{note}")
    if failures:
        print(f"gate_needs_check: {failures} self-test case(s) failed", file=sys.stderr)
        return 1
    print(f"gate_needs_check: {len(cases)} self-test cases passed")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    event = os.environ.get("GITHUB_EVENT_NAME", "pull_request").strip() or "pull_request"
    cpp = os.environ.get("CPP_CHANGED", "").strip().lower() == "true"
    web = os.environ.get("WEB_CHANGED", "").strip().lower() == "true"
    try:
        needs = json.load(sys.stdin)
    except ValueError as exc:
        print(f"gate_needs_check: FAILED CLOSED - could not read needs: {exc}", file=sys.stderr)
        return 2
    if not needs:
        print("gate_needs_check: FAILED CLOSED - empty needs", file=sys.stderr)
        return 2
    return check(needs, cpp, web, event)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
