#!/usr/bin/env python3
"""Gate the CLI agent journeys against a committed baseline.

`amber-bench run --driver binary --suite e2e` writes one row per journey: the
final answer, exit code, usage, tool calls and wall time. The rows are checked
against `bench/results/agent-e2e-baseline.json`:

- a baseline journey that is missing, or that passed and now fails, is a
  regression;
- prompt + completion tokens and tool calls fail above +10% per journey;
  those are deterministic (the loopback endpoint scripts the model), so a
  change there is a harness change;
- wall time is a pathology ceiling: `max(2x + 500 ms, baseline + 2000 ms)`.
  It catches a synchronous wait or a lost process, not scheduling noise;
- a journey in the report but not the baseline is added, and a new journey
  that fails is a regression (a broken scenario must not enter the corpus).

The baseline is regenerated with `make agent-e2e-baseline` after a reviewed
harness change. This is the same contract as `tools/bench_check.py`, with the
journey rows instead of the hermetic scenario report.
"""

import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPORT = os.path.join(REPO_ROOT, "artifacts", "agent-e2e.json")
DEFAULT_BASELINE = os.path.join(REPO_ROOT, "bench", "results", "agent-e2e-baseline.json")
DEFAULT_MARKDOWN = os.path.join(REPO_ROOT, "artifacts", "agent-e2e-summary.md")

TOKEN_REGRESSION = 0.10
CALL_REGRESSION = 0.10
WALL_CEILING_FACTOR = 2.0
WALL_CEILING_MS = 500
WALL_HEADROOM_MS = 2000
MIN_JOURNEYS = 10
REQUIRED = ("name", "ok", "prompt_tokens", "completion_tokens", "tool_calls", "wall_ms")


def load_json(path, what):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"agent-e2e-check: FAILED CLOSED - cannot read {what} {path}: {exc}",
              file=sys.stderr)
        return None


def rows_of(document, what):
    if not isinstance(document, dict) or document.get("mode") != "agent-e2e":
        print(f"agent-e2e-check: FAILED CLOSED - {what} is not an agent-e2e report",
              file=sys.stderr)
        return None
    rows = document.get("rows")
    if not isinstance(rows, list) or not rows:
        print(f"agent-e2e-check: FAILED CLOSED - {what} has no rows", file=sys.stderr)
        return None
    out = {}
    for row in rows:
        if not isinstance(row, dict):
            print(f"agent-e2e-check: FAILED CLOSED - {what} has a non-object row",
                  file=sys.stderr)
            return None
        missing = [key for key in REQUIRED if key not in row]
        if missing:
            print(f"agent-e2e-check: FAILED CLOSED - {what} row {row.get('name', '?')} "
                  f"is missing {', '.join(missing)}", file=sys.stderr)
            return None
        if not isinstance(row["tool_calls"], list) or not isinstance(row["ok"], bool):
            print(f"agent-e2e-check: FAILED CLOSED - {what} row {row['name']} has a bad shape",
                  file=sys.stderr)
            return None
        out[row["name"]] = row
    return out


def wall_ceiling(baseline_ms):
    return max(WALL_CEILING_FACTOR * baseline_ms + WALL_CEILING_MS,
               baseline_ms + WALL_HEADROOM_MS)


def compare(report_rows, baseline_rows):
    """(failures, additions, notes). Each failure names the journey."""
    failures = []
    additions = []
    notes = []
    for name, baseline in baseline_rows.items():
        if name not in report_rows:
            failures.append(f"{name}: missing from the report")
            continue
        row = report_rows[name]
        if not isinstance(baseline.get("ok"), bool):
            failures.append(f"{name}: baseline row has no ok flag")
            continue
        if baseline["ok"] and not row["ok"]:
            detail = "; ".join(row.get("failures") or []) or "the journey failed"
            failures.append(f"{name}: passed in the baseline and now fails ({detail})")
            continue
        if not baseline["ok"]:
            notes.append(f"{name}: baseline row failed; it must pass before it can gate")
            continue
        base_tokens = int(baseline["prompt_tokens"]) + int(baseline["completion_tokens"])
        new_tokens = int(row["prompt_tokens"]) + int(row["completion_tokens"])
        if base_tokens > 0 and new_tokens > base_tokens * (1.0 + TOKEN_REGRESSION):
            pct = 100.0 * (new_tokens - base_tokens) / base_tokens
            failures.append(f"{name}: tokens {base_tokens} -> {new_tokens} (+{pct:.1f}%)")
        base_calls = len(baseline["tool_calls"])
        new_calls = len(row["tool_calls"])
        if base_calls > 0 and new_calls > base_calls * (1.0 + CALL_REGRESSION):
            failures.append(f"{name}: tool calls {base_calls} -> {new_calls}")
        if baseline["tool_calls"] != row["tool_calls"]:
            notes.append(f"{name}: tool sequence changed: "
                         f"{baseline['tool_calls']} -> {row['tool_calls']}")
        ceiling = wall_ceiling(int(baseline["wall_ms"]))
        if int(row["wall_ms"]) > ceiling:
            failures.append(f"{name}: wall {row['wall_ms']} ms above ceiling {ceiling:.0f} ms")
    for name, row in report_rows.items():
        if name in baseline_rows:
            continue
        additions.append(name)
        if not row["ok"]:
            detail = "; ".join(row.get("failures") or []) or "the journey failed"
            failures.append(f"{name}: new journey fails ({detail})")
    return failures, additions, notes


def markdown_table(report_rows, baseline_rows, failures, additions):
    lines = ["| journey | tokens | tool calls | wall | verdict |", "|---|---:|---:|---:|---|"]
    failed = {failure.split(":", 1)[0] for failure in failures}
    for name in sorted(report_rows):
        row = report_rows[name]
        base = baseline_rows.get(name)
        verdict = "new"
        if name in failed:
            verdict = "FAIL"
        elif base is not None:
            verdict = "pass"
        tokens = int(row["prompt_tokens"]) + int(row["completion_tokens"])
        wall = int(row["wall_ms"])
        lines.append(f"| {name} | {tokens} | {len(row['tool_calls'])} | {wall} ms | {verdict} |")
    if additions:
        lines.append("")
        lines.append("New journeys: " + ", ".join(sorted(additions)))
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", default=DEFAULT_REPORT)
    parser.add_argument("--baseline", default=DEFAULT_BASELINE)
    parser.add_argument("--markdown", default=DEFAULT_MARKDOWN)
    parser.add_argument("--update", action="store_true",
                        help="write the report as the new baseline")
    args = parser.parse_args(argv)

    if args.update:
        report = load_json(args.report, "report")
        rows = rows_of(report, "report") if report is not None else None
        if rows is None:
            return 2
        try:
            with open(args.baseline, "w", encoding="utf-8") as handle:
                json.dump(report, handle, indent=2, sort_keys=True)
                handle.write("\n")
        except OSError as exc:
            print(f"agent-e2e-check: cannot write {args.baseline}: {exc}", file=sys.stderr)
            return 2
        print(f"agent-e2e-check: wrote {len(rows)} journey(s) to {args.baseline}")
        return 0

    report = load_json(args.report, "report")
    baseline = load_json(args.baseline, "baseline")
    if report is None or baseline is None:
        return 2
    report_rows = rows_of(report, "report")
    baseline_rows = rows_of(baseline, "baseline")
    if report_rows is None or baseline_rows is None:
        return 2
    if len(report_rows) < MIN_JOURNEYS:
        print(f"agent-e2e-check: FAILED CLOSED - {len(report_rows)} journey(s) is below the "
              f"{MIN_JOURNEYS}-journey floor", file=sys.stderr)
        return 2
    if len(baseline_rows) < MIN_JOURNEYS:
        print(f"agent-e2e-check: FAILED CLOSED - baseline has {len(baseline_rows)} journey(s), "
              f"below the {MIN_JOURNEYS}-journey floor", file=sys.stderr)
        return 2
    failed_baseline = [name for name, row in baseline_rows.items() if not row["ok"]]
    if failed_baseline:
        print("agent-e2e-check: FAILED CLOSED - the baseline contains a failing journey: "
              + ", ".join(sorted(failed_baseline)), file=sys.stderr)
        return 2

    failures, additions, notes = compare(report_rows, baseline_rows)
    summary = markdown_table(report_rows, baseline_rows, failures, additions)
    if args.markdown:
        try:
            with open(args.markdown, "w", encoding="utf-8") as handle:
                handle.write(summary)
        except OSError as exc:
            print(f"agent-e2e-check: cannot write {args.markdown}: {exc}", file=sys.stderr)
            return 2
    for note in notes:
        print(f"agent-e2e-check: note: {note}")
    if additions:
        print(f"agent-e2e-check: {len(additions)} additive journey(s): "
              + ", ".join(sorted(additions)))
    if failures:
        for failure in failures:
            print(f"agent-e2e-check: {failure}", file=sys.stderr)
        print(f"agent-e2e-check: FAILED - {len(failures)} regression(s); regenerate the "
              "baseline only after review", file=sys.stderr)
        return 1
    print(f"agent-e2e-check: {len(report_rows)} journey(s), {len(additions)} additive, "
          "no regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
