"""Tests for the CI watchdog policy and its issue bookkeeping.

The watchdog's only dangerous operations are cancel/rerun and issue writes, so
the decisions are pure and the client is faked here. Nothing in this module
touches the network.
"""

import contextlib
import io
import unittest
from datetime import datetime, timedelta, timezone

from harness import load

wd = load("ci_watchdog")

NOW = datetime(2026, 11, 12, 12, 0, tzinfo=timezone.utc)
QUEUED = timedelta(minutes=20)
PROGRESS = timedelta(minutes=90)


def make_run(
    *,
    status="in_progress",
    conclusion=None,
    name="CI",
    attempt=1,
    branch="feature/x",
    sha="deadbeef",
    created_min=0,
    updated_min=0,
    prs=(),
    run_id=1,
):
    return wd.Run(
        id=run_id,
        name=name,
        status=status,
        conclusion=conclusion,
        created_at=NOW - timedelta(minutes=created_min),
        updated_at=NOW - timedelta(minutes=updated_min),
        head_sha=sha,
        run_attempt=attempt,
        html_url=f"https://example.test/runs/{run_id}",
        event="pull_request",
        head_branch=branch,
        pull_requests=tuple(prs),
    )


def make_job(status="in_progress", name="lint", started_min=1):
    started = None if status == "queued" else NOW - timedelta(minutes=started_min)
    return wd.Job(id=1, name=name, status=status, conclusion=None, started_at=started)


class FakeClient:
    def __init__(self, runs, jobs=None, issues=None, after_cancel=None):
        self._runs = list(runs)
        self._jobs = jobs or {}
        self.issues = list(issues or [])
        self.after_cancel = after_cancel
        self.calls = []

    def runs(self, per_page=50):
        return list(self._runs)

    def run(self, run_id):
        if self.after_cancel is not None and ("cancel", run_id) in self.calls:
            return self.after_cancel
        return next(run for run in self._runs if run.id == run_id)

    def jobs(self, run_id):
        return list(self._jobs.get(run_id, []))

    def cancel(self, run_id):
        self.calls.append(("cancel", run_id))

    def rerun(self, run_id):
        self.calls.append(("rerun", run_id))

    def open_issues(self, label):
        return [issue for issue in self.issues if label in issue.labels]

    def create_issue(self, title, body, labels):
        issue = wd.Issue(len(self.issues) + 1, title, body, labels=tuple(labels))
        self.issues.append(issue)
        self.calls.append(("create", title))
        return issue

    def comment(self, issue, body):
        self.calls.append(("comment", issue))

    def update_issue(self, issue, body):
        self.calls.append(("update", issue))

    def close(self, issue):
        self.calls.append(("close", issue))

    def ensure_label(self, name, color, description):
        self.calls.append(("label", name))


class Silent(unittest.TestCase):
    """The watchdog logs decisions; tests assert, so keep the output clean."""

    def setUp(self):
        self._redirect = contextlib.redirect_stdout(io.StringIO())
        self._redirect.__enter__()

    def tearDown(self):
        self._redirect.__exit__(None, None, None)


class StuckPolicy(unittest.TestCase):
    def test_queued_run_becomes_stuck_after_the_limit(self):
        run = make_run(status="queued", created_min=21)
        stuck = wd.stuck_run(run, [], NOW, QUEUED, PROGRESS)
        self.assertIsNotNone(stuck)
        self.assertIn("queued for 21m", stuck.reason)

    def test_queued_run_is_left_alone_before_the_limit(self):
        self.assertIsNone(wd.stuck_run(make_run(status="queued", created_min=19), [], NOW, QUEUED, PROGRESS))

    def test_jobs_queued_behind_a_finished_run_are_stuck(self):
        # The real incident: every job done except ci-gate, which queued and
        # never got a runner while the run stayed in_progress.
        run = make_run(status="in_progress", updated_min=21)
        jobs = [make_job("completed", "build"), make_job("queued", "ci-gate")]
        stuck = wd.stuck_run(run, jobs, NOW, QUEUED, PROGRESS)
        self.assertIsNotNone(stuck)
        self.assertIn("ci-gate", stuck.reason)

    def test_a_queued_job_beside_a_running_job_is_not_stuck_yet(self):
        run = make_run(status="in_progress", updated_min=21)
        jobs = [make_job("in_progress", "lint"), make_job("queued", "ci-gate")]
        self.assertIsNone(wd.stuck_run(run, jobs, NOW, QUEUED, PROGRESS))

    def test_a_long_running_job_is_not_stuck_under_the_progress_limit(self):
        run = make_run(status="in_progress", updated_min=50)
        self.assertIsNone(wd.stuck_run(run, [make_job("in_progress", "lint")], NOW, QUEUED, PROGRESS))

    def test_a_run_without_progress_past_the_limit_is_stuck(self):
        run = make_run(status="in_progress", updated_min=91)
        stuck = wd.stuck_run(run, [make_job("in_progress", "lint")], NOW, QUEUED, PROGRESS)
        self.assertIsNotNone(stuck)
        self.assertIn("no run progress", stuck.reason)

    def test_completed_runs_and_other_workflows_are_ignored(self):
        completed = make_run(status="completed", conclusion="success")
        self.assertIsNone(wd.stuck_run(completed, [], NOW, QUEUED, PROGRESS))
        other = make_run(name="Merge train", status="queued", created_min=99)
        self.assertIsNone(wd.stuck_run(other, [], NOW, QUEUED, PROGRESS))


class Recover(Silent):
    def test_first_stuck_attempt_is_cancelled_and_rerun(self):
        run = make_run(status="queued", created_min=30, attempt=1, prs=(7,))
        done = make_run(status="completed", conclusion="cancelled", attempt=1)
        client = FakeClient([run], after_cancel=done)
        wd.recover(
            client,
            wd.Stuck(run, "queued for 30m", ()),
            max_attempts=2,
            dry_run=False,
            pause=lambda _: None,
        )
        self.assertIn(("cancel", 1), client.calls)
        self.assertIn(("rerun", 1), client.calls)
        self.assertIn(("comment", 7), client.calls)

    def test_exhausted_attempt_opens_one_stuck_issue(self):
        run = make_run(status="queued", created_min=30, attempt=2, prs=(7,))
        client = FakeClient([run])
        stuck = wd.Stuck(run, "queued for 30m", ())
        wd.recover(client, stuck, max_attempts=2, dry_run=False, pause=lambda _: None)
        wd.recover(client, stuck, max_attempts=2, dry_run=False, pause=lambda _: None)
        self.assertEqual(sum(1 for call in client.calls if call[0] == "create"), 1)
        self.assertEqual(len(client.issues), 1)
        self.assertIn("ci-stuck", client.issues[0].labels)

    def test_a_new_attempt_gets_its_own_issue(self):
        first = make_run(status="queued", created_min=30, attempt=2)
        second = make_run(status="queued", created_min=30, attempt=3)
        client = FakeClient([first])
        wd.recover(client, wd.Stuck(first, "stuck", ()), 2, False, lambda _: None)
        wd.recover(client, wd.Stuck(second, "stuck", ()), 2, False, lambda _: None)
        self.assertEqual(len(client.issues), 2)

    def test_dry_run_never_mutates(self):
        run = make_run(status="queued", created_min=30, attempt=1, prs=(7,))
        client = FakeClient([run])
        wd.recover(client, wd.Stuck(run, "stuck", ()), 2, True, lambda _: None)
        self.assertEqual(client.calls, [])


class RedMain(Silent):
    def test_failed_main_opens_one_issue(self):
        run = make_run(status="completed", conclusion="failure", branch="main", sha="abc12345")
        jobs = [make_job("completed", "lint"), wd.Job(2, "tsan", "completed", "failure", NOW, "url")]
        client = FakeClient([run], {1: jobs})
        wd.sync_red_issue(client, run, jobs, dry_run=False)
        wd.sync_red_issue(client, run, jobs, dry_run=False)
        self.assertEqual(len(client.issues), 1)
        self.assertIn("ci-red", client.issues[0].labels)
        self.assertIn("abc12345"[:8], client.issues[0].title)
        self.assertIn("tsan", client.issues[0].body)

    def test_a_new_red_sha_comments_instead_of_opening_another_issue(self):
        first = make_run(status="completed", conclusion="failure", branch="main", sha="abc12345")
        second = make_run(status="completed", conclusion="failure", branch="main", sha="def67890")
        client = FakeClient([first])
        client.issues.append(wd.Issue(9, "old", "failed on abc12345", labels=("ci-red",)))
        wd.sync_red_issue(client, second, [], dry_run=False)
        self.assertEqual(len(client.issues), 1)
        self.assertIn(("comment", 9), client.calls)
        self.assertIn(("update", 9), client.calls)

    def test_a_timed_out_main_is_red(self):
        run = make_run(
            status="completed", conclusion="timed_out", branch="main", sha="abc12345"
        )
        client = FakeClient([run], {1: []})
        wd.sync_red_issue(client, run, [], dry_run=False)
        self.assertEqual(len(client.issues), 1)

    def test_green_main_closes_the_open_issue(self):
        run = make_run(status="completed", conclusion="success", branch="main", sha="abc12345")
        client = FakeClient([run])
        client.issues.append(wd.Issue(9, "red", "failed", labels=("ci-red",)))
        wd.sync_red_issue(client, run, [], dry_run=False)
        self.assertIn(("comment", 9), client.calls)
        self.assertIn(("close", 9), client.calls)

    def test_an_in_progress_run_does_not_close_the_issue(self):
        done = make_run(status="completed", conclusion="failure", branch="main", sha="abc12345")
        pending = make_run(status="in_progress", branch="main", sha="def67890")
        self.assertEqual(wd.latest_ci_run([pending, done]), done)
        self.assertIsNone(wd.latest_ci_run([pending]))


class Watchdog(Silent):
    def test_dry_run_reports_without_writing(self):
        run = make_run(status="queued", created_min=30, prs=(7,))
        client = FakeClient([run], {1: []})
        args = wd.parse_args(["--repo", "o/r", "--dry-run"])
        self.assertEqual(wd.run_watchdog(args, client, NOW, lambda _: None), 0)
        self.assertEqual(client.calls, [])

    def test_a_stuck_pr_run_is_recovered(self):
        run = make_run(status="queued", created_min=30, prs=(7,))
        done = make_run(status="completed", conclusion="cancelled", attempt=1)
        client = FakeClient([run], {1: []}, after_cancel=done)
        args = wd.parse_args(["--repo", "o/r"])
        wd.run_watchdog(args, client, NOW, lambda _: None)
        self.assertIn(("cancel", 1), client.calls)
        self.assertIn(("rerun", 1), client.calls)


if __name__ == "__main__":
    unittest.main()
