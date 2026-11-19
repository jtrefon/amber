#!/usr/bin/env python3
"""Gate the policy-zoo score separation against a committed baseline.

`amber-bench run --policy-matrix --scenario zoo` runs the fixed policy zoo over
the `zoo` scenario. The binary already checks `bench/policies/expectations.json`
(which policies succeed, which must fail, and their score bounds); this gate
adds the measurement that a point expectation cannot express: the gap between
the best and the worst policy must not collapse. A matrix where every policy
scores the same has stopped discriminating even when each policy still lands on
the right side of its own threshold.

The baseline records the rows from a reviewed run. A scenario or policy that
disappears fails closed; a new one is additive. The score gap may shrink by at
most 10% (with a 0.5-point absolute slack for score arithmetic).
"""

import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPORT = os.path.join(REPO_ROOT, "artifacts", "policy-matrix.json")
DEFAULT_BASELINE = os.path.join(REPO_ROOT, "bench", "results", "policy-matrix-baseline.json")
DEFAULT_MARKDOWN = os.path.join(REPO_ROOT, "artifacts", "policy-matrix-summary.md")

SEPARATION_SLACK = 0.10
ABSOLUTE_SLACK = 0.5


def load(path, what):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"policy-matrix-check: FAILED CLOSED - cannot read {what} {path}: {exc}",
              file=sys.stderr)
        return None


def rows_of(document, what):
    if not isinstance(document, dict) or document.get("mode") != "policy-matrix":
        print(f"policy-matrix-check: FAILED CLOSED - {what} is not a policy-matrix report",
              file=sys.stderr)
        return None
    rows = document.get("rows")
    if not isinstance(rows, list) or not rows:
        print(f"policy-matrix-check: FAILED CLOSED - {what} has no rows", file=sys.stderr)
        return None
    out = {}
    for row in rows:
        if not isinstance(row, dict) or not row.get("scenario") or not row.get("policy"):
            print(f"policy-matrix-check: FAILED CLOSED - {what} has a malformed row",
                  file=sys.stderr)
            return None
        try:
            score = float(row["score"])
        except (KeyError, TypeError, ValueError):
            print(f"policy-matrix-check: FAILED CLOSED - {what} row "
                  f"{row.get('scenario')}/{row.get('policy')} has no numeric score",
                  file=sys.stderr)
            return None
        if not isinstance(row.get("success"), bool):
            print(f"policy-matrix-check: FAILED CLOSED - {what} row "
                  f"{row['scenario']}/{row['policy']} has no success flag", file=sys.stderr)
            return None
        out[(row["scenario"], row["policy"])] = dict(row, score=score)
    return out


def gaps(rows):
    by_scenario = {}
    for (scenario, policy), row in rows.items():
        by_scenario.setdefault(scenario, {})[policy] = row
    out = {}
    for scenario, policies in by_scenario.items():
        scores = [row["score"] for row in policies.values()]
        out[scenario] = {
            "gap": max(scores) - min(scores),
            "policies": policies,
        }
    return out


def compare(report_rows, baseline_rows):
    failures = []
    additions = []
    notes = []
    current = gaps(report_rows)
    recorded = gaps(baseline_rows)
    for scenario, base in recorded.items():
        if scenario not in current:
            failures.append(f"{scenario}: missing from the report")
            continue
        now = current[scenario]
        for policy, base_row in base["policies"].items():
            if policy not in now["policies"]:
                failures.append(f"{scenario}/{policy}: missing from the report")
                continue
            row = now["policies"][policy]
            if base_row["success"] and not row["success"]:
                detail = "; ".join(row.get("failures") or []) or "the policy failed"
                failures.append(f"{scenario}/{policy}: passed in the baseline and now fails "
                                f"({detail})")
        floor = base["gap"] * (1.0 - SEPARATION_SLACK) - ABSOLUTE_SLACK
        if now["gap"] < floor:
            failures.append(f"{scenario}: score separation {now['gap']:.1f} below "
                            f"{base['gap']:.1f} baseline (floor {floor:.1f})")
        if abs(now["gap"] - base["gap"]) > 0.01:
            notes.append(f"{scenario}: separation {base['gap']:.1f} -> {now['gap']:.1f}")
    for scenario, now in current.items():
        if scenario in recorded:
            continue
        additions.append(scenario)
        if any(not row["success"] for row in now["policies"].values()):
            failures.append(f"{scenario}: new scenario's matrix has a failing policy")
    return failures, additions, notes


def markdown_table(current, recorded, failures):
    lines = ["| scenario | policies | separation | baseline | verdict |",
             "|---|---:|---:|---:|---|"]
    failed = {failure.split(":", 1)[0] for failure in failures}
    for scenario in sorted(current):
        now = current[scenario]
        base = recorded.get(scenario)
        verdict = "new"
        if scenario in failed:
            verdict = "FAIL"
        elif base is not None:
            verdict = "pass"
        lines.append(f"| {scenario} | {len(now['policies'])} | {now['gap']:.1f} | "
                     f"{base['gap']:.1f} | {verdict} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", default=DEFAULT_REPORT)
    parser.add_argument("--baseline", default=DEFAULT_BASELINE)
    parser.add_argument("--markdown", default=DEFAULT_MARKDOWN)
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args(argv)

    document = load(args.report, "report")
    rows = rows_of(document, "report") if document is not None else None
    if rows is None:
        return 2
    if args.update:
        try:
            with open(args.baseline, "w", encoding="utf-8") as handle:
                json.dump(document, handle, indent=2, sort_keys=True)
                handle.write("\n")
        except OSError as exc:
            print(f"policy-matrix-check: cannot write {args.baseline}: {exc}", file=sys.stderr)
            return 2
        print(f"policy-matrix-check: wrote {len(rows)} row(s) to {args.baseline}")
        return 0

    baseline_document = load(args.baseline, "baseline")
    baseline_rows = rows_of(baseline_document, "baseline") if baseline_document else None
    if baseline_rows is None:
        return 2
    current = gaps(rows)
    recorded = gaps(baseline_rows)
    failures, additions, notes = compare(rows, baseline_rows)
    summary = markdown_table(current, recorded, failures)
    if args.markdown:
        try:
            with open(args.markdown, "w", encoding="utf-8") as handle:
                handle.write(summary)
        except OSError as exc:
            print(f"policy-matrix-check: cannot write {args.markdown}: {exc}", file=sys.stderr)
            return 2
    for note in notes:
        print(f"policy-matrix-check: note: {note}")
    if additions:
        print("policy-matrix-check: additive scenario(s): " + ", ".join(sorted(additions)))
    if failures:
        for failure in failures:
            print(f"policy-matrix-check: {failure}", file=sys.stderr)
        print(f"policy-matrix-check: FAILED - {len(failures)} regression(s)", file=sys.stderr)
        return 1
    print(f"policy-matrix-check: {len(current)} scenario(s), no separation regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
