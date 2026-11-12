"""Tests for the repeated-test driver and its quarantine contract.

The driver's dangerous operation is turning a failure into an issue, so the
GitHub client is faked here. The commands are seeded scripts that fail on a
chosen iteration: a flake the driver must name, not a test rerun that hides it.
"""

import contextlib
import io
import json
import stat
import tempfile
import unittest
from pathlib import Path

from harness import load

fr = load("flake_repeat")
wd = load("ci_watchdog")

FLAKY = """#!/usr/bin/env python3
import pathlib, sys
counter = pathlib.Path(sys.argv[1])
count = int(counter.read_text()) if counter.exists() else 0
count += 1
counter.write_text(str(count))
if count == 2:
    print("[ FAIL ] seeded_flake: boom")
    sys.exit(1)
print("[ PASS ] seeded_flake")
"""

DETERMINISTIC = """#!/usr/bin/env python3
import sys
print("[ FAIL ] seeded_hard_failure: always")
sys.exit(1)
"""


class FakeClient:
    def __init__(self):
        self.issues = []
        self.comments = []
        self.labels = []

    def ensure_label(self, name, color, description):
        self.labels.append(name)

    def open_issues(self, label):
        return [issue for issue in self.issues if label in issue.labels]

    def create_issue(self, title, body, labels):
        issue = wd.Issue(len(self.issues) + 1, title, body, labels=tuple(labels))
        self.issues.append(issue)
        return issue

    def comment(self, issue, body):
        self.comments.append((issue, body))


class Silent(unittest.TestCase):
    def setUp(self):
        self._redirect = contextlib.redirect_stdout(io.StringIO())
        self._redirect.__enter__()

    def tearDown(self):
        self._redirect.__exit__(None, None, None)


class Repeat(Silent):
    def setUp(self):
        super().setUp()
        self._tmp = tempfile.TemporaryDirectory(prefix="flake-repeat-test-")
        self.root = Path(self._tmp.name)
        self.quarantine = self.root / "quarantine.json"

    def tearDown(self):
        self._tmp.cleanup()
        super().tearDown()

    def script(self, name, body):
        path = self.root / name
        path.write_text(body, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return f"{path} {self.root / (name + '.count')}"
    def quarantine_with(self, entry):
        self.quarantine.write_text(json.dumps({"quarantine": [entry]}), encoding="utf-8")

    def test_a_seeded_flake_is_detected_and_named(self):
        summary = fr.run_suite([self.script("flaky.py", FLAKY)], 3, self.quarantine, 10)
        failure = summary.failures[("flaky.py", "seeded_flake")]
        self.assertEqual(failure.runs, [2])
        self.assertEqual(fr.kind(failure, 3), "flake")
        self.assertEqual(fr.report_failures(summary), 1)

    def test_a_deterministic_failure_is_not_called_a_flake(self):
        summary = fr.run_suite([self.script("hard.py", DETERMINISTIC)], 3, self.quarantine, 10)
        failure = summary.failures[("hard.py", "seeded_hard_failure")]
        self.assertEqual(failure.runs, [1, 2, 3])
        self.assertEqual(fr.kind(failure, 3), "deterministic")

    def test_a_quarantined_test_is_skipped_with_a_reason(self):
        self.quarantine_with(
            {
                "binary": "flaky.py",
                "test": "seeded_flake",
                "issue": 42,
                "reason": "known macOS scheduler delay",
            }
        )
        summary = fr.run_suite([self.script("flaky.py", FLAKY)], 3, self.quarantine, 10)
        self.assertTrue(summary.failures[("flaky.py", "seeded_flake")].quarantined)
        self.assertEqual(fr.report_failures(summary), 0)

    def test_a_stale_entry_is_reported(self):
        self.quarantine_with(
            {"binary": "gone.py", "test": "gone", "issue": 7, "reason": "no longer runs"}
        )
        summary = fr.run_suite([self.script("flaky.py", FLAKY)], 2, self.quarantine, 10)
        self.assertIn("gone.py:gone", summary.stale)

    def test_an_entry_without_a_reason_is_a_config_error(self):
        self.quarantine_with({"binary": "x", "test": "y", "issue": 1, "reason": ""})
        with self.assertRaises(ValueError):
            fr.load_quarantine(self.quarantine)

    def test_an_entry_without_an_issue_is_a_config_error(self):
        self.quarantine_with({"binary": "x", "test": "y", "issue": 0, "reason": "why"})
        with self.assertRaises(ValueError):
            fr.load_quarantine(self.quarantine)

    def test_the_report_names_the_flake_and_the_iteration(self):
        summary = fr.run_suite([self.script("flaky.py", FLAKY)], 3, self.quarantine, 10)
        markdown = self.root / "report.md"
        js = self.root / "report.json"
        fr.write_report(summary, markdown, js)
        text = markdown.read_text(encoding="utf-8")
        self.assertIn("seeded_flake", text)
        self.assertIn("1/3", text)
        payload = json.loads(js.read_text(encoding="utf-8"))
        self.assertEqual(payload["failures"][0]["runs"], [2])


class Issues(Silent):
    def setUp(self):
        super().setUp()
        self._tmp = tempfile.TemporaryDirectory(prefix="flake-issues-test-")
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()
        super().tearDown()

    def report(self, quarantined=False):
        path = self.root / "report.json"
        path.write_text(
            json.dumps(
                {
                    "failures": [
                        {
                            "binary": "plugin_test",
                            "test": "plugin_disable_cancels_a_hung_call",
                            "kind": "flake",
                            "runs": [2],
                            "quarantined": quarantined,
                            "detail": "FAIL: elapsed",
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        return path

    def test_one_issue_is_opened_and_then_commented_on(self):
        client = FakeClient()
        report = self.report()
        fr.record_issues(client, report)
        fr.record_issues(client, report)
        self.assertEqual(len(client.issues), 1)
        self.assertEqual(len(client.comments), 1)
        self.assertIn("flake-quarantine", client.issues[0].labels)

    def test_a_quarantined_failure_opens_nothing(self):
        client = FakeClient()
        fr.record_issues(client, self.report(quarantined=True))
        self.assertEqual(client.issues, [])


if __name__ == "__main__":
    unittest.main()
