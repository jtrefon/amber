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

ALWAYS = "always"
PULL_REQUEST_ONLY = "pull_request"
ALL_EVENTS = ("pull_request", "push")

# jobs whose result "skipped" is acceptable when the matching output is false
DIMENSIONS = {
    "changes": ALWAYS,
    "obfuscation-guard": ALWAYS,
    "secret-scan": ALWAYS,
    "dependency-review": ALWAYS,
    "lint": "cpp",
    "build-and-test": "cpp",
    "build-and-test-macos": "cpp",
    "analyze": "cpp",
    "check": "cpp",
    "complexity": "cpp",
    "duplicates": "cpp",
    "format-check": "cpp",
    "sanitizers": "cpp",
    "tsan": "cpp",
    "fuzz": "cpp",
    "coverage": "cpp",
    "website-build": "web",
    "website-smoke": "web",
}

# The self-test builds its fixtures from THIS list, never from DIMENSIONS. Deriving them
# from the table under test makes the test agree with whatever the table says: a
# mis-mapped entry changes the fixture instead of failing, which is the "gate that
# measures nothing" shape. An independent list means a bad mapping changes behaviour.
ALL_JOBS = (
    "obfuscation-guard",
    "dependency-review",
    "secret-scan",
    "lint",
    "build-and-test",
    "build-and-test-macos",
    "lint",
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
# Jobs the workflow itself scopes to pull_request events. On a push they are skipped by
# design, and treating that as "the gate did not run" turned every push to main red --
# for three consecutive runs before anyone noticed, because ci-gate is usually watched on
# pull requests. The skip is legitimate; the event is the reason.
EVENTS = {
    "dependency-review": (PULL_REQUEST_ONLY,),
    "lint": (PULL_REQUEST_ONLY,),
}

CPP_JOBS = ("build-and-test", "build-and-test-macos", "analyze", "check",
               "complexity", "duplicates", "format-check", "sanitizers", "tsan", "fuzz",
               "coverage")
WEB_JOBS = ("website-build", "website-smoke")
ALWAYS_JOBS = ("obfuscation-guard", "secret-scan")
PR_ONLY_JOBS = tuple(sorted(EVENTS))

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
        different event than the one running. `lint` is pull_request-only because its
        incremental selector is a diff selector; there is no push-shaped equivalent, and
        `lint-full` is the safety net there.
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
    """
    missing = sorted(set(DIMENSIONS) - set(ALL_JOBS) - {"changes"})
    if missing:
        print(f"gate_needs_check: jobs in DIMENSIONS but not in the self-test fixture: "
              f"{missing}", file=sys.stderr)
        return 1
    return 0


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
