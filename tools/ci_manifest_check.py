#!/usr/bin/env python3
"""Compare the CI workflow with the gate manifest.

The failure this exists to prevent: a job is added to `ci.yml`, reports a check
run, and is simply never listed in `ci-gate.needs`. `ci-gate` then judges a run
that never included it, and a red gate merges. `gate_needs_check.py` cannot see
that job either: it only judges the jobs that appear in `needs`.

The check is structural and fail-closed:

  * every job defined in `ci.yml` is in `tools/gates_manifest.py`;
  * every manifest job is defined in `ci.yml` (a stale entry is drift too);
  * every gating job is in `ci-gate.needs`, and nothing non-gating is aggregated;
  * every gating job has a local command or a stated reason it is CI-only;
  * the scan found a plausible number of jobs (a parser that matched nothing
    must not report a clean inventory).

Usage:
  tools/ci_manifest_check.py [ci.yml]
"""

import os
import re
import sys

import gates_manifest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WORKFLOW = os.path.join(REPO_ROOT, ".github", "workflows", "ci.yml")

# A parser that silently matches nothing would call an empty inventory clean.
# The workflow has well over this many jobs; the bound exists to catch a change
# in indentation or section naming, not to police the count.
MIN_JOBS = 15
MIN_NEEDS = 10

JOB = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
CI_GATE = re.compile(r"^  ci-gate:\s*$")
NEEDS = re.compile(r"^    needs:\s*(.*)$")

DIMENSIONS = ("always", "cpp", "web")


def parse_jobs(text):
    """Job ids under `jobs:`, ignoring the `on:` keys that share the indent."""
    lines = text.splitlines()
    if "jobs:" not in lines:
        return []
    start = lines.index("jobs:")
    jobs = []
    for line in lines[start + 1:]:
        if line and not line.startswith(" "):
            break
        match = JOB.match(line)
        if match:
            jobs.append(match.group(1))
    return jobs


def parse_needs(text):
    """The bracketed `needs:` list of the `ci-gate` job."""
    lines = text.splitlines()
    in_gate = False
    buffer = None
    for line in lines:
        if CI_GATE.match(line):
            in_gate = True
            continue
        if not in_gate:
            continue
        if line and not line.startswith("    "):
            break
        match = NEEDS.match(line)
        if match is not None:
            buffer = match.group(1)
        elif buffer is not None:
            buffer += " " + line.strip()
        if buffer is not None and "]" in buffer:
            start = buffer.find("[")
            end = buffer.find("]")
            if start >= 0 and end > start:
                buffer = buffer[start + 1:end]
            break
    if not buffer:
        return []
    if "[" in buffer:
        start = buffer.find("[")
        end = buffer.find("]")
        if start >= 0 and end > start:
            buffer = buffer[start + 1:end]
        else:
            return []
    return [part.strip() for part in buffer.split(",") if part.strip()]


def check(manifest, ci_jobs, needs, min_jobs=0, min_needs=0):
    """Every way the inventory can disagree with the workflow."""
    errors = []
    if not manifest:
        return ["scan sanity: the manifest is empty"]
    if len(ci_jobs) < min_jobs:
        errors.append(
            f"scan sanity: found {len(ci_jobs)} job(s), expected at least {min_jobs}"
        )
    if len(needs) < min_needs:
        errors.append(
            f"scan sanity: found {len(needs)} ci-gate need(s), expected at least {min_needs}"
        )

    known = set(manifest)
    gating = {job for job, row in manifest.items() if row["gating"]}

    for job in ci_jobs:
        if job not in known:
            errors.append(f"ci.yml job {job!r} is missing from tools/gates_manifest.py")
    for job in sorted(known - set(ci_jobs)):
        errors.append(f"manifest job {job!r} is not defined in ci.yml")

    if "changes" not in needs:
        errors.append("ci-gate.needs does not include 'changes'")
    if "ci-gate" in needs:
        errors.append("ci-gate.needs must not include itself")

    for job in sorted(gating):
        if job not in needs:
            errors.append(f"gating job {job!r} is not in ci-gate.needs")
    for job in needs:
        if job not in known:
            errors.append(f"ci-gate.needs names {job!r}, which is not in the manifest")
        elif not manifest[job]["gating"] and job != "changes":
            errors.append(f"ci-gate.needs aggregates non-gating job {job!r}")

    for job, row in manifest.items():
        if not row["gating"]:
            if not row.get("reason"):
                errors.append(f"non-gating job {job!r} has no reason")
            continue
        if row.get("dimension") not in DIMENSIONS:
            errors.append(f"gating job {job!r} has dimension {row.get('dimension')!r}")
        if not row.get("events"):
            errors.append(f"gating job {job!r} has no events")
        if not row.get("local") and not row.get("local_reason"):
            errors.append(
                f"gating job {job!r} has neither a local command nor a local_reason"
            )
    return errors


def load_and_check(path=DEFAULT_WORKFLOW):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    return check(gates_manifest.JOBS, parse_jobs(text), parse_needs(text),
                 min_jobs=MIN_JOBS, min_needs=MIN_NEEDS)


def main(argv):
    path = argv[0] if argv else DEFAULT_WORKFLOW
    try:
        errors = load_and_check(path)
    except OSError as exc:
        print(f"ci-manifest: FAILED CLOSED - could not read {path}: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("ci-manifest: FAILED")
        for error in errors:
            print(f"  {error}")
        print("  The inventory is tools/gates_manifest.py; update it and ci-gate.needs "
              "together.")
        return 1
    print(f"ci-manifest: clean ({len(gates_manifest.JOBS)} job(s), "
          f"{len(gates_manifest.gating_jobs())} gating)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
