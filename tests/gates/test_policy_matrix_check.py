"""The policy-zoo separation gate.

The binary checks point expectations; this comparator adds the score gap between
the best and worst policy, so a matrix that stopped discriminating fails even
when each policy lands on the right side of its own threshold.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

policy = load("policy_matrix_check")

POLICIES = {
    "planner": (True, 100.0),
    "lazy": (False, 60.0),
    "wrong_tool": (True, 93.0),
    "hallucinator": (False, 58.0),
    "quitter": (False, 52.5),
    "over_eager": (False, 60.0),
    "retrier": (True, 88.0),
}


def row(policy_name, success, score, scenario="zoo"):
    return {"scenario": scenario, "policy": policy_name, "success": success, "score": score,
            "recoveries": 0, "tool_failures": 0, "failures": [] if success else ["failed"]}


def rows(overrides=None, drop=None, extra=None):
    items = []
    for name, (success, score) in POLICIES.items():
        if name == drop:
            continue
        value = (overrides or {}).get(name, (success, score))
        items.append(row(name, value[0], value[1]))
    for item in extra or []:
        items.append(row(*item))
    return items


def document(items):
    return {"mode": "policy-matrix", "rows": items}


def run_main(current, baseline, update=False):
    with tempfile.TemporaryDirectory() as tmp:
        report_path = os.path.join(tmp, "report.json")
        baseline_path = os.path.join(tmp, "baseline.json")
        markdown_path = os.path.join(tmp, "summary.md")
        with open(report_path, "w", encoding="utf-8") as handle:
            json.dump(current, handle)
        with open(baseline_path, "w", encoding="utf-8") as handle:
            json.dump(baseline, handle)
        out, err = io.StringIO(), io.StringIO()
        args = ["--report", report_path, "--baseline", baseline_path,
                "--markdown", markdown_path]
        if update:
            args.append("--update")
        with redirect_stdout(out), redirect_stderr(err):
            rc = policy.main(args)
        markdown = ""
        if os.path.exists(markdown_path):
            with open(markdown_path, encoding="utf-8") as handle:
                markdown = handle.read()
        return rc, out.getvalue() + err.getvalue(), markdown


class Comparison(unittest.TestCase):
    def checked(self, overrides=None, drop=None, extra=None):
        return policy.compare(
            {(item["scenario"], item["policy"]): item
             for item in rows(overrides, drop, extra)},
            {(item["scenario"], item["policy"]): item for item in rows()})

    def test_equal_matrix_passes(self):
        failures, additions, notes = self.checked()
        self.assertEqual(failures, [])
        self.assertEqual(additions, [])
        self.assertEqual(notes, [])

    def test_a_collapsed_separation_fails(self):
        current = {name: (success, 80.0) for name, (success, _) in POLICIES.items()}
        failures, _, _ = self.checked(current)
        self.assertEqual(len(failures), 1)
        self.assertIn("separation", failures[0])

    def test_a_smaller_gap_within_the_slack_passes(self):
        # Baseline gap 47.5; floor is 42.25. A 43-point gap still discriminates.
        current = {"planner": (True, 95.0), "quitter": (False, 52.0)}
        failures, _, _ = self.checked(current)
        self.assertEqual(failures, [])

    def test_a_missing_policy_fails(self):
        failures, _, _ = self.checked(drop="retrier")
        self.assertEqual(len(failures), 1)
        self.assertIn("retrier", failures[0])
        self.assertIn("missing", failures[0])

    def test_a_policy_that_now_fails_is_a_regression(self):
        failures, _, _ = self.checked({"planner": (False, 100.0)})
        self.assertEqual(len(failures), 1)
        self.assertIn("planner", failures[0])
        self.assertIn("now fails", failures[0])

    def test_a_new_policy_is_additive(self):
        failures, additions, _ = self.checked(extra=[("new_policy", True, 90.0)])
        self.assertEqual(failures, [])
        self.assertEqual(additions, [])

    def test_a_new_scenario_with_a_failing_policy_fails(self):
        extra = [("new_policy", False, 90.0, "new_scenario")]
        failures, additions, _ = self.checked(extra=extra)
        self.assertEqual(additions, ["new_scenario"])
        self.assertEqual(len(failures), 1)
        self.assertIn("new_scenario", failures[0])


class Main(unittest.TestCase):
    def test_equal_matrix_passes_and_renders_the_summary(self):
        rc, output, markdown = run_main(document(rows()), document(rows()))
        self.assertEqual(rc, 0, output)
        self.assertIn("| scenario | policies | separation |", markdown)
        self.assertIn("zoo", markdown)

    def test_missing_report_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = policy.main(["--report", os.path.join(tmp, "missing.json"),
                                  "--baseline", os.path.join(tmp, "baseline.json"),
                                  "--markdown", ""])
            self.assertEqual(rc, 2)
            self.assertIn("FAILED CLOSED", out.getvalue() + err.getvalue())

    def test_a_report_without_scores_fails_closed(self):
        broken = document([{"scenario": "zoo", "policy": "planner", "success": True}])
        rc, output, _ = run_main(broken, document(rows()))
        self.assertEqual(rc, 2)
        self.assertIn("FAILED CLOSED", output)

    def test_update_writes_the_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path = os.path.join(tmp, "report.json")
            baseline_path = os.path.join(tmp, "baseline.json")
            with open(report_path, "w", encoding="utf-8") as handle:
                json.dump(document(rows()), handle)
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = policy.main(["--report", report_path, "--baseline", baseline_path,
                                  "--markdown", "", "--update"])
            self.assertEqual(rc, 0)
            with open(baseline_path, encoding="utf-8") as handle:
                written = json.load(handle)
            self.assertEqual(written["mode"], "policy-matrix")

    def test_a_regression_makes_the_gate_red(self):
        current = document(rows({"planner": (False, 100.0)}))
        rc, output, _ = run_main(current, document(rows()))
        self.assertEqual(rc, 1, output)
        self.assertIn("planner", output)


if __name__ == "__main__":
    unittest.main()
