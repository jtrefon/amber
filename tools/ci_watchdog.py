#!/usr/bin/env python3
"""CI watchdog: recover stuck runs and keep one tracked issue for red main.

GitHub starts a job's `timeout-minutes` only when a runner picks it up. A job
that never starts has no timeout, and a queued job still holds the workflow's
concurrency slot, so a re-run can queue behind it forever. This script detects
that state from the API and either cancels/re-runs once or opens one issue.

The policy is pure and fixture-tested; the API client is a thin shell so the
tests never touch the network.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any, Iterable, Protocol
from urllib import error, request

API_ROOT = "https://api.github.com"
WATCHED_WORKFLOWS = {"CI", "CodeQL"}
RED_CONCLUSIONS = {"failure", "timed_out"}
LABELS = {
    "ci-red": ("B60205", "main CI is failing"),
    "ci-stuck": ("D93F0B", "a workflow run did not make progress"),
}


def parse_time(value: str | None) -> datetime | None:
    if not value:
        return None
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def minutes(value: timedelta) -> str:
    total = int(value.total_seconds() // 60)
    return f"{total}m"


@dataclass(frozen=True)
class Job:
    id: int
    name: str
    status: str
    conclusion: str | None
    started_at: datetime | None
    html_url: str = ""


@dataclass(frozen=True)
class Run:
    id: int
    name: str
    status: str
    conclusion: str | None
    created_at: datetime
    updated_at: datetime
    head_sha: str
    run_attempt: int
    html_url: str
    event: str = ""
    head_branch: str = ""
    pull_requests: tuple[int, ...] = ()


@dataclass(frozen=True)
class Stuck:
    run: Run
    reason: str
    jobs: tuple[Job, ...]


@dataclass
class Issue:
    number: int
    title: str
    body: str
    state: str = "open"
    labels: tuple[str, ...] = ()


def stuck_run(
    run: Run,
    jobs: Iterable[Job],
    now: datetime,
    queued_limit: timedelta,
    progress_limit: timedelta,
) -> Stuck | None:
    """The one decision the watchdog makes about a run, with no I/O."""
    if run.name not in WATCHED_WORKFLOWS or run.status not in {"queued", "in_progress"}:
        return None
    job_list = tuple(jobs)
    if run.status == "queued":
        if now - run.created_at >= queued_limit:
            return Stuck(run, f"run queued for {minutes(now - run.created_at)}", job_list)
        return None

    pending = tuple(job for job in job_list if job.status != "completed")
    if not pending:
        # The run says in_progress but every job is done: the API is wedged.
        if now - run.updated_at >= progress_limit:
            return Stuck(run, f"no job progress for {minutes(now - run.updated_at)}", job_list)
        return None

    queued = tuple(job for job in pending if job.status == "queued" and job.started_at is None)
    if len(queued) == len(pending) and now - run.updated_at >= queued_limit:
        names = ", ".join(job.name for job in queued[:3])
        return Stuck(run, f"job(s) queued for {minutes(now - run.updated_at)}: {names}", job_list)
    if now - run.updated_at >= progress_limit:
        return Stuck(run, f"no run progress for {minutes(now - run.updated_at)}", job_list)
    return None


class Client(Protocol):
    def runs(self, per_page: int = 50) -> list[Run]: ...
    def run(self, run_id: int) -> Run: ...
    def jobs(self, run_id: int) -> list[Job]: ...
    def cancel(self, run_id: int) -> None: ...
    def rerun(self, run_id: int) -> None: ...
    def open_issues(self, label: str) -> list[Issue]: ...
    def create_issue(self, title: str, body: str, labels: list[str]) -> Issue: ...
    def comment(self, issue: int, body: str) -> None: ...
    def update_issue(self, issue: int, body: str) -> None: ...
    def close(self, issue: int) -> None: ...
    def ensure_label(self, name: str, color: str, description: str) -> None: ...


class GitHub:
    def __init__(self, repo: str, token: str, api_root: str = API_ROOT):
        self.repo = repo
        self.token = token
        self.api_root = api_root.rstrip("/")

    def _request(self, method: str, path: str, body: Any = None) -> Any:
        data = None if body is None else json.dumps(body).encode("utf-8")
        req = request.Request(
            f"{self.api_root}{path}",
            data=data,
            method=method,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "Content-Type": "application/json",
            },
        )
        try:
            with request.urlopen(req, timeout=30) as response:
                raw = response.read()
        except error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")[:300]
            raise RuntimeError(f"{method} {path}: HTTP {exc.code}: {detail}") from exc
        if not raw:
            return None
        return json.loads(raw)

    @staticmethod
    def _run(payload: dict[str, Any]) -> Run:
        return Run(
            id=payload["id"],
            name=payload.get("name") or payload.get("workflow_name") or "",
            status=payload["status"],
            conclusion=payload.get("conclusion"),
            created_at=parse_time(payload["created_at"]),
            updated_at=parse_time(payload["updated_at"]),
            head_sha=payload.get("head_sha", ""),
            run_attempt=int(payload.get("run_attempt", 1)),
            html_url=payload.get("html_url", ""),
            event=payload.get("event", ""),
            head_branch=payload.get("head_branch", ""),
            pull_requests=tuple(
                item["number"] for item in payload.get("pull_requests", []) if "number" in item
            ),
        )

    @staticmethod
    def _job(payload: dict[str, Any]) -> Job:
        return Job(
            id=payload["id"],
            name=payload.get("name", ""),
            status=payload["status"],
            conclusion=payload.get("conclusion"),
            started_at=parse_time(payload.get("started_at")),
            html_url=payload.get("html_url", ""),
        )

    def runs(self, per_page: int = 50) -> list[Run]:
        payload = self._request("GET", f"/repos/{self.repo}/actions/runs?per_page={per_page}")
        return [self._run(item) for item in payload.get("workflow_runs", [])]

    def run(self, run_id: int) -> Run:
        return self._run(self._request("GET", f"/repos/{self.repo}/actions/runs/{run_id}"))

    def jobs(self, run_id: int) -> list[Job]:
        payload = self._request(
            "GET", f"/repos/{self.repo}/actions/runs/{run_id}/jobs?per_page=100"
        )
        return [self._job(item) for item in payload.get("jobs", [])]

    def cancel(self, run_id: int) -> None:
        self._request("POST", f"/repos/{self.repo}/actions/runs/{run_id}/cancel")

    def rerun(self, run_id: int) -> None:
        self._request("POST", f"/repos/{self.repo}/actions/runs/{run_id}/rerun")

    def open_issues(self, label: str) -> list[Issue]:
        payload = self._request(
            "GET", f"/repos/{self.repo}/issues?state=open&labels={label}&per_page=20"
        )
        return [
            Issue(
                item["number"],
                item["title"],
                item.get("body") or "",
                labels=tuple(label["name"] for label in item.get("labels", [])),
            )
            for item in payload
            if "pull_request" not in item
        ]

    def create_issue(self, title: str, body: str, labels: list[str]) -> Issue:
        payload = self._request(
            "POST", f"/repos/{self.repo}/issues", {"title": title, "body": body, "labels": labels}
        )
        return Issue(
            payload["number"],
            payload["title"],
            payload.get("body") or "",
            labels=tuple(label["name"] for label in payload.get("labels", [])),
        )

    def comment(self, issue: int, body: str) -> None:
        self._request("POST", f"/repos/{self.repo}/issues/{issue}/comments", {"body": body})

    def update_issue(self, issue: int, body: str) -> None:
        self._request("PATCH", f"/repos/{self.repo}/issues/{issue}", {"body": body})

    def close(self, issue: int) -> None:
        self._request("PATCH", f"/repos/{self.repo}/issues/{issue}", {"state": "closed"})

    def ensure_label(self, name: str, color: str, description: str) -> None:
        try:
            self._request(
                "POST",
                f"/repos/{self.repo}/labels",
                {"name": name, "color": color, "description": description},
            )
        except RuntimeError:
            # 422 means it already exists; anything else is retried on the next tick.
            pass


def issue_marker(run: Run) -> str:
    return f"<!-- ci-watchdog:run:{run.id}:attempt:{run.run_attempt} -->"


def failed_jobs(jobs: Iterable[Job]) -> list[Job]:
    return [job for job in jobs if job.conclusion not in (None, "success", "skipped")]


def notify_pull_requests(client: Client, run: Run, body: str, dry_run: bool) -> None:
    for number in run.pull_requests:
        if dry_run:
            print(f"would comment on PR #{number}: {body}")
        else:
            client.comment(number, body)


def report_stuck(client: Client, stuck: Stuck, dry_run: bool) -> None:
    marker = issue_marker(stuck.run)
    existing = client.open_issues("ci-stuck")
    for issue in existing:
        if marker in issue.body:
            return
    title = f"ci: run {stuck.run.id} is stuck ({stuck.run.name})"
    body = (
        f"{marker}\n"
        f"Run [{stuck.run.id}]({stuck.run.html_url}) on `{stuck.run.head_branch or 'unknown'}` "
        f"is stuck: {stuck.reason}.\n\n"
        f"Attempt {stuck.run.run_attempt} already exhausted the automatic recovery budget.\n"
        f"Branch: `{stuck.run.head_branch}` · SHA: `{stuck.run.head_sha[:12]}`"
    )
    if dry_run:
        print(f"would open ci-stuck issue: {title}")
        return
    client.create_issue(title, body, ["ci-stuck"])
    notify_pull_requests(
        client,
        stuck.run,
        f"ci-watchdog: this run exhausted its automatic recovery budget ({stuck.reason}). "
        f"Tracking issue opened with label `ci-stuck`.",
        dry_run,
    )


def recover(client: Client, stuck: Stuck, max_attempts: int, dry_run: bool, pause) -> None:
    if stuck.run.run_attempt >= max_attempts:
        report_stuck(client, stuck, dry_run)
        return
    if dry_run:
        print(f"would cancel and re-run {stuck.run.id}: {stuck.reason}")
        return
    print(f"cancelling {stuck.run.id}: {stuck.reason}")
    client.cancel(stuck.run.id)
    notify_pull_requests(
        client,
        stuck.run,
        f"ci-watchdog: `{stuck.run.name}` was stuck ({stuck.reason}); cancelled and "
        f"re-running automatically.",
        dry_run,
    )
    current = stuck.run
    for _ in range(30):
        current = client.run(stuck.run.id)
        if current.status == "completed":
            break
        pause(2)
    if current.status != "completed":
        print(f"run {stuck.run.id} did not cancel within 60s; next tick will retry")
        return
    if current.conclusion == "cancelled" and current.run_attempt < max_attempts:
        print(f"re-running {stuck.run.id} (attempt {current.run_attempt + 1})")
        client.rerun(stuck.run.id)
    else:
        print(f"run {stuck.run.id} ended {current.conclusion}; not re-running")


def sync_red_issue(client: Client, run: Run, jobs: list[Job], dry_run: bool) -> None:
    issues = client.open_issues("ci-red")
    if run.conclusion in RED_CONCLUSIONS:
        failing = ", ".join(job.name for job in failed_jobs(jobs)[:5]) or "unknown job"
        if issues:
            issue = issues[0]
            if run.head_sha not in issue.body:
                body = f"Still red on [`{run.head_sha[:8]}`]({run.html_url}): {failing}."
                if dry_run:
                    print(f"would update and comment on ci-red #{issue.number}: {body}")
                else:
                    client.update_issue(issue.number, f"{issue.body}\n\n{body}")
                    client.comment(issue.number, body)
            return
        title = f"ci: main is red on {run.head_sha[:8]}"
        body = (
            f"`main` CI failed on [`{run.head_sha[:12]}`]({run.html_url}).\n\n"
            f"Failing job(s): {failing}.\n\n"
            f"This issue is opened once per red main; it is updated on later red "
            f"commits and closed automatically when `main` is green again."
        )
        if dry_run:
            print(f"would open ci-red issue: {title}")
            return
        client.create_issue(title, body, ["ci-red"])
        return

    if run.conclusion == "success":
        for issue in issues:
            body = f"`main` is green on [`{run.head_sha[:8]}`]({run.html_url}); closing."
            if dry_run:
                print(f"would close ci-red #{issue.number}: {body}")
            else:
                client.comment(issue.number, body)
                client.close(issue.number)


def latest_ci_run(runs: Iterable[Run]) -> Run | None:
    for run in runs:
        if run.name == "CI" and run.head_branch == "main" and run.status == "completed":
            return run
    return None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--queued-limit-min", type=int, default=20)
    parser.add_argument("--progress-limit-min", type=int, default=90)
    parser.add_argument("--max-attempts", type=int, default=2)
    return parser.parse_args(argv)


def run_watchdog(args: argparse.Namespace, client: Client, now: datetime, pause) -> int:
    failed = False
    for name, (color, description) in LABELS.items():
        if not args.dry_run:
            client.ensure_label(name, color, description)
    runs = client.runs()
    for run in runs:
        if run.status not in {"queued", "in_progress"}:
            continue
        try:
            jobs = client.jobs(run.id)
        except RuntimeError as exc:
            print(f"ci-watchdog: jobs for run {run.id}: {exc}", file=sys.stderr)
            failed = True
            continue
        stuck = stuck_run(
            run,
            jobs,
            now,
            timedelta(minutes=args.queued_limit_min),
            timedelta(minutes=args.progress_limit_min),
        )
        if not stuck:
            continue
        try:
            recover(client, stuck, args.max_attempts, args.dry_run, pause)
        except RuntimeError as exc:
            print(f"ci-watchdog: run {run.id}: {exc}", file=sys.stderr)
            failed = True
    main_run = latest_ci_run(runs)
    if main_run:
        try:
            sync_red_issue(client, main_run, client.jobs(main_run.id), args.dry_run)
        except RuntimeError as exc:
            print(f"ci-watchdog: red-main sync: {exc}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.repo:
        print("ci-watchdog: --repo or GITHUB_REPOSITORY is required", file=sys.stderr)
        return 2
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    if not token:
        print("ci-watchdog: GH_TOKEN/GITHUB_TOKEN is required", file=sys.stderr)
        return 2
    try:
        return run_watchdog(
            args, GitHub(args.repo, token), datetime.now(timezone.utc), time.sleep
        )
    except RuntimeError as exc:
        print(f"ci-watchdog: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
