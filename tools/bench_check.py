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

A regression can be waived for one reviewed PR with a dated line in the PR body:

    bench-allow: <reason> until YYYY-MM-DD

The waiver expires (an expired or undated one fails closed) and is capped at
30 days from the run, because a waiver is a reviewed exception, not a standing
budget. It does not rewrite the baseline: the next run reports it again until
the code or the baseline changes.

A scenario outside the baseline is additive. The PR that adds it may carry it
in `bench/results/harness-efficiency-additive.json` for one reviewed window
(issue, reason, expiry of at most 30 days); the alternative — and the normal
end state — is `make bench-baseline`, which graduates it into the reviewed
baseline and prunes its additive record. An unrecorded, expired or stale
additive cell fails the gate.

Live-model prompt evaluation stays manual (`BENCHMARK.md`): this measures
harness overhead, not model quality.

Usage:
  tools/bench_check.py --report bench-efficiency.json
  tools/bench_check.py --report report.json --allow "known extra call until 2026-12-24"
  tools/bench_check.py --report report.json --update
"""

import argparse
import json
import os
import sys
from datetime import date

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO_ROOT, "bench", "results", "harness-efficiency-baseline.json")
ADDITIVE = os.path.join(REPO_ROOT, "bench", "results", "harness-efficiency-additive.json")
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
# A waiver is a reviewed exception for one PR, not a standing budget: it can
# expire at most this far out, so it must be re-argued (or fixed) at review.
MAX_WAIVER_DAYS = 30
# Additive cells get one reviewed window outside the baseline, then graduate
# (`make bench-baseline`) or are removed.
MAX_ADDITIVE_DAYS = 30


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


def parse_waiver(value, today=None):
    """(reason, expires, error) for a `bench-allow: <reason> until <date>` line.

    An empty value is no waiver. A waiver without a date, an invalid date, an
    expired date, or a date beyond MAX_WAIVER_DAYS fails closed: the gate must
    never turn an arbitrary prose reason into an unbounded override.
    """
    if not value:
        return None, None, None
    today = today or date.today()
    marker = " until "
    if marker not in value:
        return None, None, ("a bench-allow waiver needs an expiry: "
                            "`bench-allow: <reason> until YYYY-MM-DD`")
    reason, raw_date = value.rsplit(marker, 1)
    reason = reason.strip()
    if not reason:
        return None, None, "a bench-allow waiver needs a reason before its expiry"
    try:
        expires = date.fromisoformat(raw_date.strip())
    except ValueError:
        return None, None, (f"bench-allow expiry is not an ISO date: "
                            f"{raw_date.strip()!r}")
    if expires < today:
        return None, None, f"the bench-allow waiver expired on {expires.isoformat()}"
    remaining = (expires - today).days
    if remaining > MAX_WAIVER_DAYS:
        return None, None, (f"the bench-allow waiver expires {remaining} days out; "
                            f"the maximum lifetime is {MAX_WAIVER_DAYS} days")
    return reason, expires, None


def load_additive(path):
    """{scenario: {issue, reason, expires}} from the additive record file.

    A missing file is an empty record: the normal end state is no additive
    cells at all. A malformed file is a configuration error, never a silent
    empty record.
    """
    if not path or not os.path.exists(path):
        return {}, None
    payload = load_json(path, "additive record")
    if payload is None:
        return None, "cannot read the additive record"
    if not isinstance(payload, dict) or not isinstance(payload.get("cells", {}), dict):
        return None, "the additive record needs a `cells` object"
    cells = payload.get("cells", {})
    for name, entry in cells.items():
        if not isinstance(entry, dict):
            return None, f"additive record {name} is not an object"
        if not isinstance(entry.get("issue"), int) or entry["issue"] <= 0:
            return None, f"additive record {name} needs a positive issue number"
        for key in ("reason", "expires"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                return None, f"additive record {name} needs a non-empty {key}"
        try:
            date.fromisoformat(entry["expires"])
        except ValueError:
            return None, f"additive record {name} has an invalid expiry"
    return cells, None


def save_additive(path, cells):
    payload = {
        "_comment": (
            "Scenarios measured outside the hermetic baseline. An entry needs "
            "an issue, a reason and an expiry of at most 30 days from the gate "
            "run; `make bench-baseline` graduates a cell into the reviewed "
            "baseline and prunes its entry here. An unrecorded, expired or "
            "stale cell fails the gate."
        ),
        "cells": {name: cells[name] for name in sorted(cells)},
    }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def evaluate_additions(added, additive, today=None):
    """(violations, active, stale) for additive cells and their records."""
    today = today or date.today()
    violations = []
    active = {}
    for name in added:
        entry = additive.get(name)
        if entry is None:
            violations.append(
                f"{name}: additive scenario is outside the baseline and has no "
                f"additive record; graduate it with `make bench-baseline` or "
                f"record it in bench/results/harness-efficiency-additive.json")
            continue
        expires = date.fromisoformat(entry["expires"])
        if expires < today:
            violations.append(
                f"{name}: additive record (issue #{entry['issue']}) expired on "
                f"{expires.isoformat()}")
            continue
        remaining = (expires - today).days
        if remaining > MAX_ADDITIVE_DAYS:
            violations.append(
                f"{name}: additive record expires {remaining} days out; the "
                f"maximum window is {MAX_ADDITIVE_DAYS} days")
            continue
        active[name] = entry
    stale = sorted(name for name in additive if name not in added)
    for name in stale:
        violations.append(
            f"{name}: additive record is stale (the scenario graduated or no "
            f"longer runs); remove it from "
            f"bench/results/harness-efficiency-additive.json")
    return violations, active, stale


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


def markdown(results, violations, added, allow="", harness=None, waiver=None, active=None):
    lines = ["## Hermetic harness efficiency", ""]
    if harness:
        passed = harness.get("harness_passed")
        total = harness.get("harness_total")
        if passed is not None and total is not None:
            lines.append(f"Harness integrity: **{passed}/{total}**")
            lines.append("")
    if waiver:
        lines += [
            f"**Active waiver:** {waiver['reason']} (expires {waiver['expires']}, "
            f"{waiver['days']} day(s) left). Only the measured regressions are "
            f"waived; a measurement or additive-record failure is never waived.",
            "",
        ]
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
    if added:
        lines.append("")
        active = active or {}
        for name in added:
            entry = active.get(name)
            if entry is not None:
                lines.append(
                    f"Additive cell `{name}` is on a reviewed window "
                    f"(issue #{entry['issue']}, expires {entry['expires']}).")
        unrecorded = [name for name in added if name not in active]
        if unrecorded:
            lines.append("Additive scenarios without a record (fail): "
                         + ", ".join(f"`{name}`" for name in unrecorded))
    if violations:
        lines.append("")
        lines.append(f"**{len(violations)} regression(s)**"
                     + (f" — allowed by: {allow}" if allow else ""))
    return "\n".join(lines) + "\n"


def check(current, recorded, allow="", verbose=True, min_scenarios=0, additive=None,
          today=None):
    """0 pass, 1 regression, 2 the measurement cannot be trusted."""
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
    today = today or date.today()
    additive = additive or {}
    reason, expires, waiver_error = parse_waiver(allow, today)
    if waiver_error:
        print(f"bench-check: FAILED CLOSED - {waiver_error}", file=sys.stderr)
        return 2
    addition_violations, active, _ = evaluate_additions(added, additive, today)
    if verbose:
        print(f"bench-check: {len(recorded)} hermetic scenario(s) compared"
              + (f", {len(added)} additive" if added else ""))
        for kind, name, base, cur, bar in violations:
            if isinstance(base, (int, float)) and isinstance(cur, (int, float)):
                detail = f"{percent(cur, base):+.1f}%"
            else:
                detail = f"{base} -> {cur}"
            print(f"  {name}: {kind} {base} -> {cur} ({detail}, {bar})")
        for violation in addition_violations:
            print(f"  {violation}")
        for name, entry in sorted(active.items()):
            print(f"  {name}: additive window (issue #{entry['issue']}, "
                  f"expires {entry['expires']})")
        if reason and expires:
            print(f"bench-check: active waiver: {reason} "
                  f"(expires {expires.isoformat()}, {(expires - today).days} day(s) left)")
    if addition_violations:
        print(f"bench-check: FAILED - {len(addition_violations)} additive-record "
              f"violation(s); a waiver covers a measured regression, never an "
              f"additive cell.")
        return 1
    if violations and reason:
        print(f"bench-check: allowed by: {reason} "
              f"(expires {expires.isoformat()}, {(expires - today).days} day(s) left) "
              f"({len(violations)} regression(s) reported)")
        return 0
    if violations:
        print(f"bench-check: FAILED - {len(violations)} regression(s) beyond the "
              f"hermetic threshold. Fix the harness overhead, or put a dated "
              f"`bench-allow: <reason> until YYYY-MM-DD` in the PR body "
              f"(maximum {MAX_WAIVER_DAYS} days).")
        return 1
    if verbose and reason:
        print(f"bench-check: waiver not used (no regression observed): {reason} "
              f"(expires {expires.isoformat()})")
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
    ap.add_argument("--additive", default=ADDITIVE,
                    help="committed record of additive cells outside the baseline")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    ap.add_argument("--allow", default="",
                    help="`<reason> until YYYY-MM-DD` from the PR body")
    ap.add_argument("--today", default=None,
                    help="ISO date for waiver/record checks (default: today)")
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
    try:
        today = date.fromisoformat(args.today) if args.today else date.today()
    except ValueError:
        print(f"bench-check: FAILED CLOSED - --today is not an ISO date: {args.today!r}",
              file=sys.stderr)
        return 2
    additive, additive_error = load_additive(args.additive)
    if additive is None:
        print(f"bench-check: FAILED CLOSED - {additive_error}", file=sys.stderr)
        return 2
    if args.update:
        store(args.baseline, current)
        graduated = sorted(name for name in additive if name in current)
        if graduated:
            save_additive(args.additive,
                          {name: additive[name] for name in additive
                           if name not in graduated})
            print(f"bench-check: graduated {len(graduated)} additive record(s): "
                  f"{', '.join(graduated)}")
        print(f"bench-check: wrote {len(current)} scenarios to {args.baseline}")
        return 0

    baseline = load_json(args.baseline, "baseline")
    if baseline is None:
        return 2
    recorded, error = baseline_metrics(baseline)
    if recorded is None:
        print(f"bench-check: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    rc = check(current, recorded, args.allow, min_scenarios=args.min_scenarios,
               additive=additive, today=today)
    harness = None
    if "harness_passed" in report and "harness_total" in report:
        harness = {"harness_passed": report["harness_passed"],
                   "harness_total": report["harness_total"]}
        print(f"bench-check: harness integrity "
              f"{report['harness_passed']}/{report['harness_total']}")
    if args.markdown:
        violations, added, _ = compare(current, recorded)
        reason, expires, _ = parse_waiver(args.allow, today)
        waiver = None
        if reason and expires:
            waiver = {"reason": reason, "expires": expires.isoformat(),
                      "days": (expires - today).days}
        _, active, _ = evaluate_additions(added, additive, today)
        results = [(name, (recorded[name], current[name])) for name in sorted(recorded)]
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(markdown(results, violations, added, args.allow, harness,
                                  waiver=waiver, active=active))
    return rc


if __name__ == "__main__":
    sys.exit(main())
