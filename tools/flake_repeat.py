#!/usr/bin/env python3
"""Repeat the flake-prone test subset and turn a failure into a tracked issue.

A test that fails on the second or third run is a flake, even when the PR run
was green. This driver runs the plugin lifecycle and pty harness repeatedly,
attributes a failure to the test that produced it, and writes a report a
reviewer can act on.

Quarantine is an explicit list, never a silent skip: an entry needs an issue
number and a reason, and the driver prints `[ QUARANTINE ]` every time it
suppresses a failure. Removing an entry is a normal review step once the test
has been stable for a while.

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
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_QUARANTINE = REPO_ROOT / "tests" / "flake_quarantine.json"
DEFAULT_REPORT = REPO_ROOT / "artifacts" / "flake-report.md"
DEFAULT_JSON = REPO_ROOT / "artifacts" / "flake-report.json"
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

    def unquarantined(self) -> list[Failure]:
        return [failure for failure in self.failures.values() if not failure.quarantined]


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
    return entries


def quarantine_match(entries: Iterable[dict], binary: str, test: str) -> dict | None:
    for entry in entries:
        if entry["binary"] != binary:
            continue
        if entry["test"] in ("*", test):
            return entry
    return None


def run_suite(commands: list[str], repeat: int, quarantine_path: Path, timeout: int) -> Summary:
    entries = load_quarantine(quarantine_path)
    failures: dict[tuple[str, str], Failure] = {}
    for command in commands:
        binary = command_binary(command)
        for iteration in range(1, repeat + 1):
            print(f"flake-repeat: {binary} run {iteration}/{repeat}")
            code, output, timed_out = run_once(command, iteration, timeout)
            if output:
                print(output.rstrip())
            if code == 0 and not timed_out:
                continue
            name = failure_name(output) or (f"timeout after {timeout}s" if timed_out else f"exit {code}")
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
    exercised = {(failure.binary, failure.test) for failure in failures.values()}
    stale = [
        f"{entry['binary']}:{entry['test']}"
        for entry in entries
        if (entry["binary"], entry["test"]) not in exercised
    ]
    return Summary(repeat=repeat, commands=commands, failures=failures,
                   quarantined=entries, stale=stale)


def kind(failure: Failure, repeat: int) -> str:
    return "deterministic" if len(failure.runs) >= repeat else "flake"


def write_report(summary: Summary, markdown: Path, js: Path) -> None:
    markdown.parent.mkdir(parents=True, exist_ok=True)
    js.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    payload = []
    for failure in summary.failures.values():
        failure_kind = kind(failure, summary.repeat)
        state = "quarantined" if failure.quarantined else "open"
        rows.append(
            f"| {failure.binary} | {failure.test} | {failure_kind} | "
            f"{len(failure.runs)}/{summary.repeat} | {failure.runs} | {state} |"
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
        f"- repeat: {summary.repeat}",
        "- commands:",
        *[f"  - `{command}`" for command in summary.commands],
        "",
    ]
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
        lines += ["## Quarantine", "", "| binary | test | issue | reason |", "|---|---|---|---|"]
        for entry in summary.quarantined:
            lines.append(
                f"| {entry['binary']} | {entry['test']} | #{entry['issue']} | {entry['reason']} |"
            )
        lines.append("")
    if summary.stale:
        lines += ["## Stale quarantine entries", "", *[f"- {item}" for item in summary.stale], ""]
    markdown.write_text("\n".join(lines), encoding="utf-8")
    js.write_text(
        json.dumps(
            {
                "repeat": summary.repeat,
                "commands": summary.commands,
                "failures": payload,
                "quarantine": summary.quarantined,
                "stale": summary.stale,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def report_failures(summary: Summary) -> int:
    open_failures = summary.unquarantined()
    flaky = [failure for failure in open_failures if kind(failure, summary.repeat) == "flake"]
    deterministic = [failure for failure in open_failures if failure not in flaky]
    if not open_failures:
        print(
            f"flake-repeat: clean ({summary.repeat} run(s), "
            f"{len(summary.quarantined)} quarantined)"
        )
        return 0
    print(
        f"flake-repeat: FAILED - {len(open_failures)} test(s): "
        f"{len(flaky)} flaky, {len(deterministic)} deterministic"
    )
    for failure in open_failures:
        runs = ", ".join(str(run) for run in failure.runs)
        print(f"  {failure.binary}: {failure.test}: {kind(failure, summary.repeat)} "
              f"({len(failure.runs)}/{summary.repeat}, runs {runs})")
    return 1


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
            f"issue's number and a reason:\n"
            f"```json\n"
            f"{{ \"binary\": \"{failure.binary}\", \"test\": \"{failure.test}\", "
            f"\"issue\": <this issue>, \"reason\": \"...\" }}\n"
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
    parser.add_argument("--repeat", type=int,
                        default=int(os.environ.get("AMBER_TEST_REPEAT", "3")))
    parser.add_argument("--quarantine", type=Path, default=DEFAULT_QUARANTINE)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--timeout", type=int, default=1800,
                        help="per-run timeout in seconds (default 1800)")
    parser.add_argument("--open-issue", type=Path, metavar="REPORT_JSON",
                        help="open/update issues from an existing report and exit")
    return parser.parse_args(argv)


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
    if args.repeat < 1:
        print("flake-repeat: --repeat must be at least 1", file=sys.stderr)
        return 2
    try:
        summary = run_suite(args.command, args.repeat, args.quarantine, args.timeout)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"flake-repeat: {exc}", file=sys.stderr)
        return 2
    write_report(summary, args.report, args.json)
    print(f"flake-repeat: report: {args.report}")
    return report_failures(summary)


if __name__ == "__main__":
    sys.exit(main())
