#!/usr/bin/env python3
"""Render the committed hermetic report as the website's data file.

The canonical record is `bench/results/hermetic/<version>.json`, produced by
`amber-bench run --cat both`. The site imports a compact projection so it does
not carry the per-scenario KPI payload into every page build.
"""

import argparse
import json
import sys


def compact(report):
    if report.get("mode") != "hermetic":
        raise ValueError(f"report mode is {report.get('mode')!r}, not 'hermetic'")
    scenarios = report.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise ValueError("report has no scenarios")
    suites = []
    for row in report.get("suite_summary", []):
        if not isinstance(row, dict) or not row.get("suite"):
            continue
        suites.append({
            "suite": row["suite"],
            "scenarios": int(row.get("scenarios", 0)),
            "passed": int(row.get("passed", 0)),
            "tokens": int(row.get("prompt_tokens", 0)) + int(row.get("completion_tokens", 0)),
            "tool_calls": int(row.get("tool_calls", 0)),
            "wall_ms": int(row.get("wall_ms", 0)),
        })
    return {
        "schema_version": 1,
        "engine_version": str(report.get("engine_version", "")),
        "scenarios": len(scenarios),
        "passed": sum(1 for row in scenarios if row.get("success")),
        "harness_passed": int(report.get("harness_passed", 0)),
        "harness_total": int(report.get("harness_total", 0)),
        "harness_integrity": float(report.get("harness_integrity", 0.0)),
        "suites": suites,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True, help="hermetic amber-bench report")
    ap.add_argument("--out", required=True, help="website data file")
    args = ap.parse_args()
    try:
        with open(args.report, encoding="utf-8") as handle:
            report = json.load(handle)
        data = compact(report)
    except (OSError, ValueError, TypeError) as exc:
        print(f"bench-site-data: {exc}", file=sys.stderr)
        return 2
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")
    print(f"bench-site-data: wrote {data['passed']}/{data['scenarios']} scenarios "
          f"and {data['harness_passed']}/{data['harness_total']} probes to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
