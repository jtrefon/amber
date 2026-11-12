#!/usr/bin/env python3
"""Hermetic harness-efficiency gate.

`amber-bench` records prompt/completion tokens, tool calls, wall time and task
success for every scenario, but nothing compared those numbers with a baseline:
a change that doubles the harness's token use or tool calls landed green.

The hermetic (fake-LLM) scenarios are deterministic in tokens and calls, so a
change there is a harness change, not a model's mood. The gate compares a fresh
report with `bench/results/harness-efficiency-baseline.json`:

- **tokens** (prompt + completion): fail above +10% per scenario;
- **tool calls**: fail above +10% per scenario;
- **task success**: a scenario that passed in the baseline and fails now is a
  regression;
- **wall time**: reported, and failed above a 2x + 500 ms ceiling. Wall time is
  machine- and load-dependent even with a fake LLM, so it is a pathology
  ceiling rather than a precise budget; tokens and calls are the real gate.

A `bench-allow: <reason>` line in the PR body (the CI job passes it as
`--allow`) turns a regression into a reported override. It does not rewrite the
baseline: the next run reports it again until the code or the baseline changes.

Live-model prompt evaluation stays manual (`BENCHMARK.md`): this measures
harness overhead, not model quality.

Usage:
  tools/bench_check.py --report bench-efficiency.json
  tools/bench_check.py --report report.json --allow "known extra call"
  tools/bench_check.py --report report.json --update
"""

import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO_ROOT, "bench", "results", "harness-efficiency-baseline.json")
TOKEN_REGRESSION = 0.10
CALL_REGRESSION = 0.10
WALL_CEILING_FACTOR = 2.0
WALL_CEILING_MS = 500
REQUIRED_METRICS = ("prompt_tokens", "completion_tokens", "tool_calls", "wall_ms", "success")


def load_json(path, what):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"bench-check: FAILED CLOSED - cannot read {what} {path}: {exc}",
              file=sys.stderr)
        return None


def metrics(report):
    """{scenario: metrics} from a bench report, or None when it is not a report
    this gate can trust."""
    if not isinstance(report, dict) or not isinstance(report.get("scenarios"), list):
        return None
    out = {}
    for scenario in report["scenarios"]:
        if not isinstance(scenario, dict) or not scenario.get("name"):
            return None
        name = scenario["name"]
        try:
            out[name] = {
                "prompt_tokens": int(scenario.get("prompt_tokens", 0)),
                "completion_tokens": int(scenario.get("completion_tokens", 0)),
                "tool_calls": int(scenario.get("tool_calls_total", 0)),
                "wall_ms": int(scenario.get("wall_ms", 0)),
                "success": bool(scenario.get("success", False)),
            }
        except (TypeError, ValueError):
            return None
    return out


def baseline_metrics(baseline):
    """The recorded per-scenario metrics, or None when the file is malformed."""
    scenarios = baseline.get("scenarios") if isinstance(baseline, dict) else None
    if not isinstance(scenarios, dict) or not scenarios:
        return None, "the baseline has no scenarios"
    for name, values in scenarios.items():
        if not isinstance(values, dict):
            return None, f"baseline entry {name} is not an object"
        missing = [key for key in REQUIRED_METRICS if key not in values]
        if missing:
            return None, f"baseline entry {name} is missing {', '.join(missing)}"
    return scenarios, None


def percent(now, then):
    if then == 0:
        return 0.0 if now == 0 else 100.0
    return 100.0 * (now - then) / then


def compare(current, recorded):
    """(violations, added, missing) for one report against the baseline.

    `missing` is a measurement failure: the baseline names a scenario the report
    did not run, so the comparison would silently cover less than it claims.
    `added` is additive (a new scenario) and passes.
    """
    violations = []
    missing = sorted(set(recorded) - set(current))
    added = sorted(set(current) - set(recorded))
    for name in sorted(set(recorded) & set(current)):
        base = recorded[name]
        cur = current[name]
        base_tokens = base["prompt_tokens"] + base["completion_tokens"]
        cur_tokens = cur["prompt_tokens"] + cur["completion_tokens"]
        if cur_tokens > base_tokens * (1 + TOKEN_REGRESSION):
            violations.append((name, "tokens", base_tokens, cur_tokens,
                               f"bar +{TOKEN_REGRESSION * 100:.0f}%"))
        if cur["tool_calls"] > base["tool_calls"] * (1 + CALL_REGRESSION):
            violations.append((name, "tool calls", base["tool_calls"], cur["tool_calls"],
                               f"bar +{CALL_REGRESSION * 100:.0f}%"))
        ceiling = base["wall_ms"] * WALL_CEILING_FACTOR + WALL_CEILING_MS
        if cur["wall_ms"] > ceiling:
            violations.append((name, "wall time", base["wall_ms"], cur["wall_ms"],
                               f"ceiling {ceiling:.0f}ms"))
        if base["success"] and not cur["success"]:
            violations.append((name, "success", "passed", "failed", "task regression"))
    return violations, added, missing


def markdown(results, violations, added, allow=""):
    lines = ["## Hermetic harness efficiency", "",
             "| scenario | tokens | Δ | calls | Δ | wall | result |",
             "|---|---:|---:|---:|---:|---:|---|"]
    failed = {v[0] for v in violations}
    for name, values in results:
        base, cur = values
        tokens = cur["prompt_tokens"] + cur["completion_tokens"]
        base_tokens = base["prompt_tokens"] + base["completion_tokens"]
        result = "fail" if name in failed else "ok"
        lines.append(
            f"| `{name}` | {tokens} | {percent(tokens, base_tokens):+.1f}% "
            f"| {cur['tool_calls']} | {percent(cur['tool_calls'], base['tool_calls']):+.1f}% "
            f"| {cur['wall_ms']}ms | {result} |")
    if added:
        lines.append("")
        lines.append(f"Additive scenarios (no baseline): {', '.join(f'`{n}`' for n in added)}")
    if violations:
        lines.append("")
        lines.append(f"**{len(violations)} regression(s)**"
                     + (f" — allowed by: {allow}" if allow else ""))
    return "\n".join(lines) + "\n"


def check(current, recorded, allow="", verbose=True):
    """0 pass, 1 regression, 2 the measurement cannot be trusted."""
    violations, added, missing = compare(current, recorded)
    if missing:
        print(f"bench-check: FAILED CLOSED - the report did not run "
              f"{len(missing)} baseline scenario(s): {', '.join(missing)}",
              file=sys.stderr)
        return 2
    results = [(name, (recorded[name], current[name])) for name in sorted(recorded)]
    if verbose:
        print(f"bench-check: {len(recorded)} hermetic scenario(s) compared"
              + (f", {len(added)} additive" if added else ""))
        for kind, name, base, cur, bar in violations:
            if isinstance(base, (int, float)) and isinstance(cur, (int, float)):
                detail = f"{percent(cur, base):+.1f}%"
            else:
                detail = f"{base} -> {cur}"
            print(f"  {name}: {kind} {base} -> {cur} ({detail}, {bar})")
    if violations and allow:
        print(f"bench-check: allowed by: {allow} ({len(violations)} regression(s) reported)")
        return 0
    if violations:
        print(f"bench-check: FAILED - {len(violations)} regression(s) beyond the "
              f"hermetic threshold. Fix the harness overhead, or put "
              f"`bench-allow: <reason>` in the PR body.")
        return 1
    if verbose:
        print("bench-check: no regression")
    return 0


def store(path, current):
    payload = {
        "_comment": (
            "Hermetic harness-efficiency baseline: prompt/completion tokens, "
            "executed tool calls, wall time and task success per fake-LLM "
            "scenario. Regenerate with `make bench-baseline` and review the "
            "diff; the gate fails on +10% tokens or calls, a dropped success, "
            "or a 2x+500ms wall-time ceiling."
        ),
        "mode": "hermetic",
        "scenarios": {name: current[name] for name in sorted(current)},
    }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True, help="amber-bench JSON report")
    ap.add_argument("--baseline", default=BASELINE, help="committed baseline")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    ap.add_argument("--allow", default="", help="reason from `bench-allow: <reason>`")
    ap.add_argument("--markdown", help="write the summary table here")
    args = ap.parse_args()

    # The reason is user-controlled text. CI passes it through the environment
    # rather than through a shell recipe, so a backtick or quote in a PR body
    # cannot become shell syntax.
    if not args.allow:
        args.allow = os.environ.get("BENCH_ALLOW", "")
    if not args.markdown:
        args.markdown = os.environ.get("BENCH_MARKDOWN", "")

    report = load_json(args.report, "report")
    if report is None:
        return 2
    if report.get("mode") != "hermetic":
        print(f"bench-check: FAILED CLOSED - {args.report} is a "
              f"{report.get('mode', 'unknown')!r} run; this gate reads hermetic "
              f"reports only.", file=sys.stderr)
        return 2
    current = metrics(report)
    if current is None:
        print(f"bench-check: FAILED CLOSED - {args.report} has no usable scenario "
              f"metrics.", file=sys.stderr)
        return 2
    if args.update:
        store(args.baseline, current)
        print(f"bench-check: wrote {len(current)} scenarios to {args.baseline}")
        return 0

    baseline = load_json(args.baseline, "baseline")
    if baseline is None:
        return 2
    recorded, error = baseline_metrics(baseline)
    if recorded is None:
        print(f"bench-check: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    rc = check(current, recorded, args.allow)
    if args.markdown:
        violations, added, _ = compare(current, recorded)
        results = [(name, (recorded[name], current[name])) for name in sorted(recorded)]
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(markdown(results, violations, added, args.allow))
    return rc


if __name__ == "__main__":
    sys.exit(main())
