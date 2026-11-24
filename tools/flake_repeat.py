#!/usr/bin/env python3
"""Repeat the flake-prone test subset and turn a failure into a tracked issue.

A test that fails on the second or third run is a flake, even when the PR run
was green. This driver runs the plugin lifecycle and pty harness repeatedly,
attributes a failure to the test that produced it, and writes a report a
reviewer can act on.

Quarantine is an explicit list, never a silent skip: an entry needs an issue
number, an `opened` date and a reason, and the driver prints `[ QUARANTINE ]`
every time it suppresses a failure. Removing an entry is a normal review step
once the test has been stable for a while.

The numeric budget (`tests/flake_budget.json`) caps the quarantine set, the
share of repeated executions a quarantined failure may spoil, and the age of an
entry before review. Release mode also refuses a stale entry: a suppression the
release candidate no longer needs should be removed, not carried into a tag.

`--open-issue` reuses the CI watchdog's GitHub client to keep one open issue per
flaky test, so a nightly run cannot bury the same failure in a new issue each
time.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_QUARANTINE = REPO_ROOT / "tests" / "flake_quarantine.json"
DEFAULT_BUDGET = REPO_ROOT / "tests" / "flake_budget.json"
DEFAULT_REPORT = REPO_ROOT / "artifacts" / "flake-report.md"
DEFAULT_JSON = REPO_ROOT / "artifacts" / "flake-report.json"
DEFAULT_BUDGET_VALUES = {
    "repeat": 3,
    "journey_repeat": 2,
    "max_flake_rate": 0.15,
    "max_quarantined": 3,
    "quarantine_review_days": 30,
    "known_unmeasured": [],
}
FLAKE_LABEL = ("flake-quarantine", "5319E7", "a repeated test failed")
MAX_DETAIL = 2000

RUN_FAIL = re.compile(r"^\[ FAIL \] ([^\s:]+)")
RUN_TIMEOUT = re.compile(r"^\[ TIMEOUT \] ([^\s:]+)")
MINITEST_FAIL = re.compile(r"^FAIL: (.+)$")


@dataclass
class Failure:
    binary: str
    test: str
    runs: list[int] = field(default_factory=list)
    detail: str = ""
    quarantined: bool = False

    @property
    def key(self) -> str:
        return f"{self.binary}:{self.test}"


@dataclass
class Summary:
    repeat: int
    commands: list[str]
    failures: dict[tuple[str, str], Failure]
    quarantined: list[dict]
    stale: list[str]
    budget: dict = field(default_factory=dict)
    journeys: list[str] = field(default_factory=list)
    journey_repeat: int = 0

    def unquarantined(self) -> list[Failure]:
        return [failure for failure in self.failures.values() if not failure.quarantined]

    def quarantined_failures(self) -> list[Failure]:
        return [failure for failure in self.failures.values() if failure.quarantined]

    def repeat_for(self, binary: str) -> int:
        if any(command_binary(command) == binary for command in self.journeys):
            return self.journey_repeat
        return self.repeat

    @property
    def iterations(self) -> int:
        return len(self.commands) * self.repeat + len(self.journeys) * self.journey_repeat

    def _failed_iterations(self, failures: Iterable[Failure]) -> int:
        return len({(failure.binary, run) for failure in failures for run in failure.runs})

    @property
    def failed_iterations(self) -> int:
        return self._failed_iterations(self.failures.values())

    @property
    def quarantined_failed_iterations(self) -> int:
        return self._failed_iterations(self.quarantined_failures())

    @property
    def flake_rate(self) -> float:
        return self.failed_iterations / self.iterations if self.iterations else 0.0

    @property
    def quarantined_rate(self) -> float:
        return self.quarantined_failed_iterations / self.iterations if self.iterations else 0.0


def command_binary(command: str) -> str:
    return Path(shlex.split(command)[0]).name


def failure_name(output: str) -> str | None:
    """The most specific failure name the binary printed, if any."""
    for line in output.splitlines():
        match = RUN_FAIL.match(line) or RUN_TIMEOUT.match(line)
        if match:
            return match.group(1)
    for line in output.splitlines():
        match = MINITEST_FAIL.match(line)
        if match:
            return match.group(1).strip()[:160]
    return None


def run_once(command: str, iteration: int, timeout: int) -> tuple[int, str, bool]:
    try:
        proc = subprocess.run(
            shlex.split(command),
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return proc.returncode, (proc.stdout or "") + (proc.stderr or ""), False
    except subprocess.TimeoutExpired as exc:
        output = ""
        for part in (exc.stdout, exc.stderr):
            if isinstance(part, bytes):
                output += part.decode("utf-8", "replace")
            elif part:
                output += part
        return 124, output, True


def load_quarantine(path: Path) -> list[dict]:
    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(payload, list):
        entries = payload
    else:
        entries = payload.get("quarantine", [])
    if not isinstance(entries, list):
        raise ValueError("quarantine must be a list")
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("each quarantine entry must be an object")
        for key in ("binary", "test", "reason"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError(f"quarantine entry needs a non-empty {key}: {entry!r}")
        if not isinstance(entry.get("issue"), int) or entry["issue"] <= 0:
            raise ValueError(f"quarantine entry needs a positive issue number: {entry!r}")
        # The review clock needs a start date; a quarantine without one cannot
        # be aged, so it would live forever.
        opened = entry.get("opened")
        if not isinstance(opened, str):
            raise ValueError(f"quarantine entry needs an ISO 'opened' date: {entry!r}")
        try:
            date.fromisoformat(opened)
        except ValueError as exc:
            raise ValueError(f"quarantine entry has an invalid 'opened' date: {entry!r}") from exc
    return entries


def load_budget(path: Path) -> dict:
    """Load the flake budget, with the 0.9 defaults when no file is committed."""
    budget = dict(DEFAULT_BUDGET_VALUES)
    if not path.exists():
        return budget
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("flake budget must be an object")
    for key in ("repeat", "journey_repeat", "max_quarantined", "quarantine_review_days"):
        if key not in payload:
            continue
        value = payload[key]
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ValueError(f"flake budget {key} must be a non-negative integer")
        budget[key] = value
    if budget["repeat"] < 1:
        raise ValueError("flake budget repeat must be at least 1")
    if "max_flake_rate" in payload:
        rate = payload["max_flake_rate"]
        if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not 0 <= rate <= 1:
            raise ValueError("flake budget max_flake_rate must be a number in [0, 1]")
        budget["max_flake_rate"] = float(rate)
    known = payload.get("known_unmeasured", [])
    if not isinstance(known, list):
        raise ValueError("flake budget known_unmeasured must be a list")
    for entry in known:
        if not isinstance(entry, dict):
            raise ValueError("each known_unmeasured entry must be an object")
        if not isinstance(entry.get("issue"), int) or entry["issue"] <= 0:
            raise ValueError(f"known_unmeasured entry needs an issue number: {entry!r}")
        for key in ("behaviour", "reason"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError(f"known_unmeasured entry needs a non-empty {key}: {entry!r}")
    budget["known_unmeasured"] = known
    return budget


def review_state(entry: dict, today: date, review_days: int | None) -> tuple[int, bool]:
    """How old a quarantine entry is, and whether it is past its review window."""
    opened = date.fromisoformat(entry["opened"])
    age = (today - opened).days
    return age, review_days is not None and age > review_days


def quarantine_match(entries: Iterable[dict], binary: str, test: str) -> dict | None:
    for entry in entries:
        if entry["binary"] != binary:
            continue
        if entry["test"] in ("*", test):
            return entry
    return None


def run_suite(commands: list[str], repeat: int, quarantine_path: Path, timeout: int,
              budget: dict | None = None,
              journey_commands: list[str] | None = None) -> Summary:
    entries = load_quarantine(quarantine_path)
    budget = budget if budget is not None else {}
    journey_commands = journey_commands or []
    journey_repeat = int(budget.get("journey_repeat", 0)) if journey_commands else 0
    failures: dict[tuple[str, str], Failure] = {}

    def run_batch(batch: list[str], iterations: int) -> None:
        for command in batch:
            binary = command_binary(command)
            for iteration in range(1, iterations + 1):
                print(f"flake-repeat: {binary} run {iteration}/{iterations}")
                code, output, timed_out = run_once(command, iteration, timeout)
                if output:
                    print(output.rstrip())
                if code == 0 and not timed_out:
                    continue
                name = failure_name(output) or (
                    f"timeout after {timeout}s" if timed_out else f"exit {code}")
                key = (binary, name)
                failure = failures.setdefault(key, Failure(binary=binary, test=name))
                failure.runs.append(iteration)
                failure.detail = output[-MAX_DETAIL:]
                entry = quarantine_match(entries, binary, name)
                if entry is not None:
                    failure.quarantined = True
                    print(
                        f"[ QUARANTINE ] {binary}: {name} (issue #{entry['issue']}): "
                        f"{entry['reason']}"
                    )

    run_batch(commands, repeat)
    run_batch(journey_commands, journey_repeat)
    exercised = {(failure.binary, failure.test) for failure in failures.values()}
    stale = [
        f"{entry['binary']}:{entry['test']}"
        for entry in entries
        if (entry["binary"], entry["test"]) not in exercised
    ]
    return Summary(repeat=repeat, commands=commands, failures=failures,
                   quarantined=entries, stale=stale, budget=budget,
                   journeys=journey_commands, journey_repeat=journey_repeat)


def kind(failure: Failure, repeat: int) -> str:
    return "deterministic" if len(failure.runs) >= repeat else "flake"


def evaluate(summary: Summary, release: bool = False, today: date | None = None) -> list[str]:
    """Every way the run can leave its budget, as human-readable violations."""
    today = today or date.today()
    budget = summary.budget or {}
    violations: list[str] = []
    open_failures = summary.unquarantined()
    if open_failures:
        flaky = [failure for failure in open_failures
                 if kind(failure, summary.repeat_for(failure.binary)) == "flake"]
        violations.append(
            f"{len(open_failures)} unquarantined failure(s): {len(flaky)} flaky, "
            f"{len(open_failures) - len(flaky)} deterministic")
    if "max_flake_rate" in budget:
        max_rate = float(budget["max_flake_rate"])
        if summary.quarantined_failed_iterations and summary.quarantined_rate > max_rate:
            violations.append(
                f"quarantined failures on {summary.quarantined_failed_iterations}/"
                f"{summary.iterations} iterations ({summary.quarantined_rate:.1%}) exceed the "
                f"{max_rate:.1%} budget")
    if "max_quarantined" in budget and len(summary.quarantined) > int(budget["max_quarantined"]):
        violations.append(
            f"{len(summary.quarantined)} quarantine entries exceed the "
            f"{budget['max_quarantined']} limit")
    if "quarantine_review_days" in budget:
        review_days = int(budget["quarantine_review_days"])
        for entry in summary.quarantined:
            age, overdue = review_state(entry, today, review_days)
            if overdue:
                violations.append(
                    f"quarantine {entry['binary']}:{entry['test']} (issue #{entry['issue']}) "
                    f"was opened {age} days ago; review it within {review_days} days")
    if release and summary.stale:
        violations.append(
            f"{len(summary.stale)} stale quarantine entrie(s) were not exercised in the "
            f"release run; remove them or record why they must stay: "
            f"{', '.join(summary.stale)}")
    return violations


def _percent(value) -> str:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return f"{float(value):.1%}"
    return "-"


def budget_line(summary: Summary) -> str:
    budget = summary.budget or {}
    if "max_flake_rate" not in budget:
        return ""
    return (
        f"flake-repeat: budget - failed iterations {summary.failed_iterations}/"
        f"{summary.iterations} ({summary.flake_rate:.1%}); quarantined "
        f"{len(summary.quarantined)}/{budget.get('max_quarantined', '-')}; "
        f"review window {budget.get('quarantine_review_days', '-')} days; "
        f"known unmeasured {len(budget.get('known_unmeasured', []))}")


def write_report(summary: Summary, markdown: Path, js: Path, release: bool = False,
                 today: date | None = None) -> None:
    markdown.parent.mkdir(parents=True, exist_ok=True)
    js.parent.mkdir(parents=True, exist_ok=True)
    today = today or date.today()
    violations = evaluate(summary, release=release, today=today)
    review_days = summary.budget.get("quarantine_review_days")
    rows = []
    payload = []
    for failure in summary.failures.values():
        failure_kind = kind(failure, summary.repeat_for(failure.binary))
        state = "quarantined" if failure.quarantined else "open"
        rows.append(
            f"| {failure.binary} | {failure.test} | {failure_kind} | "
            f"{len(failure.runs)}/{summary.repeat_for(failure.binary)} | "
            f"{failure.runs} | {state} |"
        )
        payload.append(
            {
                "binary": failure.binary,
                "test": failure.test,
                "kind": failure_kind,
                "runs": failure.runs,
                "quarantined": failure.quarantined,
                "detail": failure.detail,
            }
        )
    lines = [
        "# Flake repeat report",
        "",
        f"- mode: {'release' if release else 'detector'}",
        f"- repeat: {summary.repeat}",
        "- commands:",
        *[f"  - `{command}`" for command in summary.commands],
    ]
    if summary.journeys:
        lines += [
            f"- journey repeat: {summary.journey_repeat}",
            "- journeys:",
            *[f"  - `{command}`" for command in summary.journeys],
        ]
    lines += [
        "",
        "## Budget",
        "",
        f"- failed iterations: {summary.failed_iterations}/{summary.iterations} "
        f"({summary.flake_rate:.1%})",
        f"- quarantined: {len(summary.quarantined)}/"
        f"{summary.budget.get('max_quarantined', '-')}",
        f"- max quarantined flake rate: "
        f"{_percent(summary.budget.get('max_flake_rate'))}",
        f"- quarantine review window: "
        f"{review_days if review_days is not None else 'not set'} days",
        f"- verdict: {'OVER BUDGET' if violations else 'within budget'}",
        "",
    ]
    if summary.budget.get("known_unmeasured"):
        lines += ["## Known unmeasured", ""]
        for entry in summary.budget["known_unmeasured"]:
            lines.append(
                f"- #{entry['issue']} {entry['behaviour']} - {entry['reason']}")
        lines.append("")
    if violations:
        lines += ["## Budget violations", "", *[f"- {item}" for item in violations], ""]
    if rows:
        lines += [
            "## Failures",
            "",
            "| binary | test | kind | runs | iterations | state |",
            "|---|---|---|---|---|---|",
            *rows,
            "",
        ]
    else:
        lines += ["## Failures", "", "None.", ""]
    if summary.quarantined:
        lines += [
            "## Quarantine",
            "",
            "| binary | test | issue | opened | age | reason |",
            "|---|---|---|---|---|---|",
        ]
        for entry in summary.quarantined:
            age, overdue = review_state(entry, today, review_days)
            marker = " (overdue)" if overdue else ""
            lines.append(
                f"| {entry['binary']} | {entry['test']} | #{entry['issue']} | "
                f"{entry['opened']} | {age}d{marker} | {entry['reason']} |"
            )
        lines.append("")
    if summary.stale:
        lines += ["## Stale quarantine entries", "", *[f"- {item}" for item in summary.stale], ""]
    markdown.write_text("\n".join(lines), encoding="utf-8")
    js.write_text(
        json.dumps(
            {
                "mode": "release" if release else "detector",
                "repeat": summary.repeat,
                "journey_repeat": summary.journey_repeat,
                "commands": summary.commands,
                "journeys": summary.journeys,
                "budget": summary.budget,
                "iterations": summary.iterations,
                "failed_iterations": summary.failed_iterations,
                "flake_rate": summary.flake_rate,
                "violations": violations,
                "failures": payload,
                "quarantine": summary.quarantined,
                "stale": summary.stale,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def report_failures(summary: Summary, release: bool = False,
                    today: date | None = None) -> int:
    line = budget_line(summary)
    if line:
        print(line)
    review_days = summary.budget.get("quarantine_review_days")
    long_lived = next((entry for entry in summary.quarantined
                       if review_days is not None and
                       review_state(entry, today or date.today(), int(review_days))[1]),
                      None)
    if long_lived is not None:
        print(f"[ REVIEW ] {long_lived['binary']}: {long_lived['test']} "
              f"(issue #{long_lived['issue']}) is past its quarantine review window")
    if summary.stale:
        print("flake-repeat: stale quarantine entries (not exercised in this run): "
              + ", ".join(summary.stale))
    violations = evaluate(summary, release=release, today=today)
    open_failures = summary.unquarantined()
    if open_failures:
        flaky = [failure for failure in open_failures
                 if kind(failure, summary.repeat_for(failure.binary)) == "flake"]
        deterministic = [failure for failure in open_failures if failure not in flaky]
        print(
            f"flake-repeat: FAILED - {len(open_failures)} test(s): "
            f"{len(flaky)} flaky, {len(deterministic)} deterministic"
        )
        for failure in open_failures:
            runs = ", ".join(str(run) for run in failure.runs)
            print(f"  {failure.binary}: {failure.test}: "
                  f"{kind(failure, summary.repeat_for(failure.binary))} "
                  f"({len(failure.runs)}/{summary.repeat_for(failure.binary)}, runs {runs})")
    if violations and not open_failures:
        print(f"flake-repeat: FAILED - {len(violations)} budget violation(s)")
        for violation in violations:
            print(f"  {violation}")
    elif violations:
        for violation in violations:
            print(f"  budget: {violation}")
    if not open_failures and not violations:
        print(
            f"flake-repeat: clean ({summary.iterations} iteration(s), "
            f"{len(summary.quarantined)} quarantined)"
        )
    return 0 if not violations else 1


def issue_marker(failure: Failure) -> str:
    digest = hashlib.sha1(failure.key.encode("utf-8")).hexdigest()[:12]
    return f"<!-- flake-repeat:{digest} -->"


def record_issues(client, report_path: Path) -> int:
    """Open or update one issue per unquarantined failure."""
    payload = json.loads(report_path.read_text(encoding="utf-8"))
    failures = [item for item in payload.get("failures", []) if not item.get("quarantined")]
    if not failures:
        print("flake-repeat: no unquarantined failures; no issue to open")
        return 0
    client.ensure_label(*FLAKE_LABEL)
    existing = client.open_issues(FLAKE_LABEL[0])
    run_url = os.environ.get("RUN_URL", "")
    for item in failures:
        failure = Failure(binary=item["binary"], test=item["test"])
        marker = issue_marker(failure)
        body = (
            f"{marker}\n"
            f"The repeated test job failed on `{failure.binary}: {failure.test}` "
            f"({item.get('kind', 'flake')}, runs {item.get('runs')}).\n\n"
            f"Run: {run_url}\n\n"
            f"```\n{item.get('detail', '')[-MAX_DETAIL:]}\n```\n\n"
            f"To quarantine, add this to `tests/flake_quarantine.json` with this "
            f"issue's number, the date and a reason:\n"
            f"```json\n"
            f"{{ \"binary\": \"{failure.binary}\", \"test\": \"{failure.test}\", "
            f"\"issue\": <this issue>, \"opened\": \"{date.today().isoformat()}\", "
            f"\"reason\": \"...\" }}\n"
            f"```\n"
        )
        matched = next((issue for issue in existing if marker in issue.body), None)
        if matched:
            client.comment(matched.number, f"Still failing. {run_url}")
            print(f"flake-repeat: commented on issue #{matched.number} for {failure.key}")
        else:
            issue = client.create_issue(
                f"flake: {failure.test} failed the repeat job ({failure.binary})",
                body,
                [FLAKE_LABEL[0]],
            )
            print(f"flake-repeat: opened issue #{issue.number} for {failure.key}")
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", action="append", default=[],
                        help="command to repeat (may be given more than once)")
    parser.add_argument("--journey-command", action="append", default=[],
                        help="journey command to repeat at the budget's journey_repeat "
                             "(may be given more than once)")
    parser.add_argument("--repeat", type=int,
                        default=int(os.environ.get("AMBER_TEST_REPEAT", "0")) or None,
                        help="subset repeat count (default: AMBER_TEST_REPEAT or the budget)")
    parser.add_argument("--quarantine", type=Path, default=DEFAULT_QUARANTINE)
    parser.add_argument("--budget", type=Path, default=DEFAULT_BUDGET)
    parser.add_argument("--release", action="store_true",
                        help="release mode: a stale quarantine entry is a failure")
    parser.add_argument("--today", type=str, default=None,
                        help="ISO date for quarantine age checks (default: today)")
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--timeout", type=int, default=1800,
                        help="per-run timeout in seconds (default 1800)")
    parser.add_argument("--open-issue", type=Path, metavar="REPORT_JSON",
                        help="open/update issues from an existing report and exit")
    return parser.parse_args(argv)


def parse_today(value: str | None) -> date:
    if value is None:
        return date.today()
    try:
        return date.fromisoformat(value)
    except ValueError as exc:
        raise ValueError(f"--today must be an ISO date: {value}") from exc


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.open_issue is not None:
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
        repo = os.environ.get("GITHUB_REPOSITORY", "")
        if not token or not repo:
            print("flake-repeat: GH_TOKEN/GITHUB_TOKEN and GITHUB_REPOSITORY are required",
                  file=sys.stderr)
            return 2
        from ci_watchdog import GitHub

        try:
            return record_issues(GitHub(repo, token), args.open_issue)
        except (OSError, ValueError, RuntimeError) as exc:
            print(f"flake-repeat: {exc}", file=sys.stderr)
            return 1
    if not args.command:
        print("flake-repeat: at least one --command is required", file=sys.stderr)
        return 2
    try:
        budget = load_budget(args.budget)
        repeat = args.repeat if args.repeat is not None else budget["repeat"]
        today = parse_today(args.today)
        if repeat < 1:
            raise ValueError("--repeat must be at least 1")
        summary = run_suite(args.command, repeat, args.quarantine, args.timeout,
                            budget=budget, journey_commands=args.journey_command)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"flake-repeat: {exc}", file=sys.stderr)
        return 2
    write_report(summary, args.report, args.json, release=args.release, today=today)
    print(f"flake-repeat: report: {args.report}")
    return report_failures(summary, release=args.release, today=today)


if __name__ == "__main__":
    sys.exit(main())
