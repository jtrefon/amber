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
  regression; a new scenario that fails is a regression too;
- **corpus floor**: a report below 30 scenarios fails closed, so a gate that
  measured less than it claims does not report a pass;
- **wall time**: reported, and failed above a 2x + 500 ms ceiling, raised to a
  minimum of baseline + 2000 ms. Wall time is machine- and load-dependent even
  with a fake LLM, so it is a pathology ceiling rather than a precise budget;
  tokens and calls are the real gate. The absolute headroom exists because a
  short scenario's 2x ceiling is smaller than ordinary CI scheduling noise
  (measured: `t-07-planning-multi-file`, 113 ms baseline, 763-1012 ms on loaded
  runners).

A `bench-allow: <reason> until YYYY-MM-DD` line in the PR body (the CI job
passes it as `--allow`) turns a regression into a reported override for at most
14 days. The date is required: a waiver without an expiry, an expired one, or
one too far out fails. It does not rewrite the baseline: the next run reports
it again until the code or the baseline changes.

A scenario in the report but not in the baseline is an **additive cell**. It
passes while it is recorded in `tests/bench_additive.json` with an `opened`
date and an issue, and it must join the baseline or be removed within
`max_age_days`; an unrecorded, stale or overdue entry fails the gate. This is
the graduation rule: a cell cannot stay outside the baseline indefinitely
without a recorded decision.

Live-model prompt evaluation stays manual (`BENCHMARK.md`): this measures
harness overhead, not model quality.

Usage:
  tools/bench_check.py --report bench-efficiency.json
  tools/bench_check.py --report report.json --allow "known extra call"
  tools/bench_check.py --report report.json --update
"""

import argparse
import datetime
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO_ROOT, "bench", "results", "harness-efficiency-baseline.json")
ADDITIVE_REGISTER = os.path.join(REPO_ROOT, "tests", "bench_additive.json")
TOKEN_REGRESSION = 0.10
CALL_REGRESSION = 0.10
WALL_CEILING_FACTOR = 2.0
WALL_CEILING_MS = 500
# Absolute headroom for short scenarios: 2x of a 113 ms baseline is 226 ms,
# less than one scheduling slice on a shared runner. The floor keeps the gate
# a pathology check; tokens and calls remain the precise axes.
WALL_HEADROOM_MS = 2000
REQUIRED_METRICS = ("prompt_tokens", "completion_tokens", "tool_calls", "wall_ms", "success")
# The hermetic corpus floor. A report below it measured less than the gate
# claims, even when every scenario it did run passed.
MIN_SCENARIOS = 30
# A waiver is a short, dated exception reviewed in the PR, not a permanent
# quieting. The author names an expiry; the reviewer approves it; the gate
# refuses a missing, past or too-distant date.
WAIVER_MAX_DAYS = 14
# An additive cell is reported for one reviewed PR cycle, then it must join the
# baseline (graduation) or be removed. The register records when it entered.
ADDITIVE_MAX_AGE_DAYS = 30


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
                "suite": str(scenario.get("suite", "")),
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


def tokens_measured(values):
    """Whether a scenario reported any token usage at all.

    The long-horizon policy client drives real tools through a scripted policy
    and reports no usage counters, so its token cells are not measurements of
    zero. They are absence of measurement and must be shown as such, never
    gated as a free pass or summarised as a 0-token cost."""
    return values["prompt_tokens"] > 0 or values["completion_tokens"] > 0


def parse_waiver(text, today=None):
    """(reason, expiry, error) from `reason until YYYY-MM-DD`.

    Empty text is no waiver. A waiver is deliberately short-lived: the date is
    required, must not be in the past, and must be within WAIVER_MAX_DAYS.
    """
    text = (text or "").strip()
    if not text:
        return "", None, ""
    match = re.search(r"\s+until\s+(\d{4}-\d{2}-\d{2})\s*$", text)
    if not match:
        return text, None, ("a bench-allow waiver needs an expiry: "
                            "`bench-allow: <reason> until YYYY-MM-DD`")
    reason = text[:match.start()].strip()
    if not reason:
        return text, None, "a bench-allow waiver needs a reason before `until`"
    try:
        expiry = datetime.date.fromisoformat(match.group(1))
    except ValueError:
        return text, None, f"bench-allow expiry is not a date: {match.group(1)}"
    today = today or datetime.date.today()
    if expiry < today:
        return reason, expiry, f"the bench-allow waiver expired on {expiry.isoformat()}"
    if (expiry - today).days > WAIVER_MAX_DAYS:
        return reason, expiry, (f"a bench-allow waiver may last at most "
                                f"{WAIVER_MAX_DAYS} days (expiry {expiry.isoformat()})")
    return reason, expiry, ""


def load_additive(path):
    """The additive register, or None when it cannot be trusted."""
    register = load_json(path, "additive register")
    if register is None:
        return None
    if not isinstance(register, dict) or not isinstance(register.get("cells"), dict):
        print(f"bench-check: FAILED CLOSED - {path} has no cells map", file=sys.stderr)
        return None
    return register


def additive_problems(added, register, today=None):
    """Why the additive cells are not graduated, as report lines.

    Every cell outside the baseline is a decision that has not been made yet.
    It is allowed while the register names it and its age is within the limit;
    a cell that joined the baseline (or vanished) leaves a stale entry behind,
    which is a failure so the register stays exact.
    """
    today = today or datetime.date.today()
    try:
        max_age = int(register.get("max_age_days", ADDITIVE_MAX_AGE_DAYS))
    except (TypeError, ValueError):
        return [f"the additive register has an invalid max_age_days"]
    cells = register.get("cells", {})
    problems = []
    for name in added:
        entry = cells.get(name)
        if not isinstance(entry, dict) or not entry.get("opened") or not entry.get("issue"):
            problems.append(
                f"{name}: additive cell is not in tests/bench_additive.json "
                f"(record opened + issue, or graduate it into the baseline)")
            continue
        try:
            opened = datetime.date.fromisoformat(str(entry["opened"]))
        except ValueError:
            problems.append(f"{name}: additive entry has no valid opened date")
            continue
        age = (today - opened).days
        if age > max_age:
            problems.append(
                f"{name}: additive since {opened.isoformat()} ({age}d > {max_age}d); "
                f"graduate it into the baseline or remove it")
    for name in sorted(cells):
        if name not in added:
            problems.append(
                f"{name}: registered as additive but it is in the baseline or did "
                f"not run; remove the register entry")
    return problems


def compare(current, recorded):
    """(violations, added, missing) for one report against the baseline.

    `missing` is a measurement failure: the baseline names a scenario the report
    did not run, so the comparison would silently cover less than it claims.
    `added` is a new scenario. A passing addition is a note; a failing one is a
    regression, because the corpus must not grow a red scenario unnoticed.
    """
    violations = []
    missing = sorted(set(recorded) - set(current))
    added = sorted(set(current) - set(recorded))
    for name in added:
        if not current[name]["success"]:
            violations.append((name, "success", "new scenario", "failed", "additive scenario"))
    for name in sorted(set(recorded) & set(current)):
        base = recorded[name]
        cur = current[name]
        base_tokens = base["prompt_tokens"] + base["completion_tokens"]
        cur_tokens = cur["prompt_tokens"] + cur["completion_tokens"]
        if (tokens_measured(base) or tokens_measured(cur)) and \
                cur_tokens > base_tokens * (1 + TOKEN_REGRESSION):
            violations.append((name, "tokens", base_tokens, cur_tokens,
                               f"bar +{TOKEN_REGRESSION * 100:.0f}%"))
        if cur["tool_calls"] > base["tool_calls"] * (1 + CALL_REGRESSION):
            violations.append((name, "tool calls", base["tool_calls"], cur["tool_calls"],
                               f"bar +{CALL_REGRESSION * 100:.0f}%"))
        ceiling = max(base["wall_ms"] * WALL_CEILING_FACTOR + WALL_CEILING_MS,
                      base["wall_ms"] + WALL_HEADROOM_MS)
        if cur["wall_ms"] > ceiling:
            violations.append((name, "wall time", base["wall_ms"], cur["wall_ms"],
                               f"ceiling {ceiling:.0f}ms"))
        if base["success"] and not cur["success"]:
            violations.append((name, "success", "passed", "failed", "task regression"))
    return violations, added, missing


def suite_summary(results):
    """Per-suite totals for the run page. `results` is (base, current) pairs."""
    agg = {}
    for _, (base, cur) in results:
        suite = cur.get("suite") or "?"
        row = agg.setdefault(suite, {
            "scenarios": 0, "passed": 0, "tokens": 0, "base_tokens": 0,
            "measured": 0, "calls": 0, "base_calls": 0, "wall": 0,
        })
        row["scenarios"] += 1
        row["passed"] += 1 if cur["success"] else 0
        row["measured"] += 1 if tokens_measured(cur) else 0
        row["tokens"] += cur["prompt_tokens"] + cur["completion_tokens"]
        row["base_tokens"] += base["prompt_tokens"] + base["completion_tokens"]
        row["calls"] += cur["tool_calls"]
        row["base_calls"] += base["tool_calls"]
        row["wall"] += cur["wall_ms"]
    return agg


def markdown(results, violations, added, allow="", harness=None, waiver=None):
    lines = ["## Hermetic harness efficiency", ""]
    if harness:
        passed = harness.get("harness_passed")
        total = harness.get("harness_total")
        if passed is not None and total is not None:
            lines.append(f"Harness integrity: **{passed}/{total}**")
            lines.append("")
    lines += ["| suite | pass | tokens | Δ | calls | Δ | wall |",
              "|---|---:|---:|---:|---:|---:|---:|"]
    unmeasured = []
    for suite, row in sorted(suite_summary(results).items()):
        if row["measured"] or row["base_tokens"]:
            tokens = (f"{row['tokens']} | "
                      f"{percent(row['tokens'], row['base_tokens']):+.1f}%")
        else:
            tokens = "n/a | n/a"
            unmeasured.append(f"`{suite}`")
        lines.append(
            f"| `{suite}` | {row['passed']}/{row['scenarios']} "
            f"| {tokens} "
            f"| {row['calls']} | {percent(row['calls'], row['base_calls']):+.1f}% "
            f"| {row['wall']}ms |")
    lines += ["", "| scenario | tokens | Δ | calls | Δ | wall | result |",
              "|---|---:|---:|---:|---:|---:|---|"]
    failed = {v[0] for v in violations}
    for name, values in results:
        base, cur = values
        tokens = cur["prompt_tokens"] + cur["completion_tokens"]
        base_tokens = base["prompt_tokens"] + base["completion_tokens"]
        if tokens_measured(cur) or tokens_measured(base):
            token_cell = (f"{tokens} | {percent(tokens, base_tokens):+.1f}%")
        else:
            token_cell = "n/a | n/a"
        result = "fail" if name in failed else "ok"
        lines.append(
            f"| `{name}` | {token_cell} "
            f"| {cur['tool_calls']} | {percent(cur['tool_calls'], base['tool_calls']):+.1f}% "
            f"| {cur['wall_ms']}ms | {result} |")
    if unmeasured:
        lines += ["",
                  f"Token cost is not measured for {', '.join(unmeasured)} "
                  "(outcome-only paths: no usage counters are reported). Those "
                  "cells are `n/a`, not zero, and the gate covers outcome, tool "
                  "calls and wall time there."]
    if waiver:
        reason, expiry = waiver
        lines += ["",
                  f"Waiver: {reason} (until {expiry.isoformat()}); "
                  "reviewed in the PR, expires on that date."]
    if added:
        failing = [n for n in added if n in failed]
        passing = [n for n in added if n not in failed]
        lines.append("")
        if failing:
            lines.append(f"Additive scenarios that failed: "
                         f"{', '.join(f'`{n}`' for n in failing)}")
        if passing:
            lines.append(f"Additive scenarios (no baseline): "
                         f"{', '.join(f'`{n}`' for n in passing)}")
    if violations:
        lines.append("")
        lines.append(f"**{len(violations)} regression(s)**"
                     + (f" — allowed by: {allow}" if allow else ""))
    return "\n".join(lines) + "\n"


def check(current, recorded, allow="", verbose=True, min_scenarios=0, today=None,
          additive=None):
    """0 pass, 1 regression or policy failure, 2 the measurement cannot be trusted."""
    if len(current) < min_scenarios:
        print(f"bench-check: FAILED CLOSED - the report ran {len(current)} "
              f"scenario(s), below the corpus floor of {min_scenarios}",
              file=sys.stderr)
        return 2
    violations, added, missing = compare(current, recorded)
    if missing:
        print(f"bench-check: FAILED CLOSED - the report did not run "
              f"{len(missing)} baseline scenario(s): {', '.join(missing)}",
              file=sys.stderr)
        return 2
    if additive is None:
        additive = load_additive(ADDITIVE_REGISTER)
        if additive is None:
            return 2
    additive_issues = additive_problems(added, additive, today)
    if additive_issues:
        print("bench-check: FAILED - additive cells need a recorded graduation decision:",
              file=sys.stderr)
        for problem in additive_issues:
            print(f"  {problem}", file=sys.stderr)
        return 1
    reason, expiry, waiver_error = parse_waiver(allow, today)
    if waiver_error:
        print(f"bench-check: FAILED - {waiver_error}", file=sys.stderr)
        return 1
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
    if violations and reason:
        print(f"bench-check: allowed by: {reason} until {expiry.isoformat()} "
              f"({len(violations)} regression(s) reported)")
        return 0
    if violations:
        print(f"bench-check: FAILED - {len(violations)} regression(s) beyond the "
              f"hermetic threshold. Fix the harness overhead, or put "
              f"`bench-allow: <reason>` in the PR body.")
        return 1
    if verbose:
        if reason:
            print(f"bench-check: waiver active: {reason} until {expiry.isoformat()} "
                  f"(no regression to waive)")
        print("bench-check: no regression")
    return 0


def store(path, current):
    payload = {
        "_comment": (
            "Hermetic harness-efficiency baseline: prompt/completion tokens, "
            "executed tool calls, wall time and task success per fake-LLM "
            "scenario. Regenerate with `make bench-baseline` and review the "
            "diff; the gate fails on +10% tokens or calls, a dropped success, "
            "or a 2x+500ms wall-time ceiling. A scenario with no usage counters "
            "is outcome-only: its token cell is n/a, not a measured zero."
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
    ap.add_argument("--graduate", action="store_true",
                    help="add every additive cell to the baseline, keeping the recorded "
                         "cells byte-identical")
    ap.add_argument("--allow", default="",
                    help="reason from `bench-allow: <reason> until YYYY-MM-DD`")
    ap.add_argument("--additive-register", default=ADDITIVE_REGISTER,
                    help="committed register of additive cells outside the baseline")
    ap.add_argument("--markdown", help="write the summary table here")
    ap.add_argument("--min-scenarios", type=int, default=MIN_SCENARIOS,
                    help=f"fail closed below this corpus size (default {MIN_SCENARIOS})")
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
    if args.graduate:
        baseline = load_json(args.baseline, "baseline")
        if baseline is None:
            return 2
        recorded, error = baseline_metrics(baseline)
        if recorded is None:
            print(f"bench-check: FAILED CLOSED - {error}", file=sys.stderr)
            return 2
        added = sorted(set(current) - set(recorded))
        if not added:
            print("bench-check: no additive cells to graduate")
            return 0
        merged = dict(recorded)
        for name in added:
            merged[name] = current[name]
        store(args.baseline, merged)
        print(f"bench-check: graduated {len(added)} additive cell(s) into "
              f"{args.baseline}: {', '.join(added)}")
        return 0

    baseline = load_json(args.baseline, "baseline")
    if baseline is None:
        return 2
    recorded, error = baseline_metrics(baseline)
    if recorded is None:
        print(f"bench-check: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    additive = load_additive(args.additive_register)
    if additive is None:
        return 2
    rc = check(current, recorded, args.allow, min_scenarios=args.min_scenarios,
               additive=additive)
    harness = None
    if "harness_passed" in report and "harness_total" in report:
        harness = {"harness_passed": report["harness_passed"],
                   "harness_total": report["harness_total"]}
        print(f"bench-check: harness integrity "
              f"{report['harness_passed']}/{report['harness_total']}")
    if args.markdown:
        violations, added, _ = compare(current, recorded)
        reason, expiry, _ = parse_waiver(args.allow)
        results = [(name, (recorded[name], current[name])) for name in sorted(recorded)]
        waiver = (reason, expiry) if reason and expiry else None
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(markdown(results, violations, added, args.allow, harness, waiver))
    return rc


if __name__ == "__main__":
    sys.exit(main())
