"""The CLI agent-journey baseline gate.

The loopback endpoint scripts the model, so tokens and tool calls are
deterministic; wall time is a pathology ceiling. The comparator is tested with
fixtures so a real regression does not have to be committed.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

agent = load("agent_e2e_check")

NAMES = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
         "india", "juliet"]


def row(name, prompt=100, completion=10, calls=None, wall=100, ok=True, failures=None):
    return {
        "name": name,
        "ok": ok,
        "prompt_tokens": prompt,
        "completion_tokens": completion,
        "tool_calls": list(calls if calls is not None else ["read"]),
        "wall_ms": wall,
        "exit_code": 0,
        "final_text": "done" if ok else "",
        "failures": list(failures or []),
    }


def rows(**overrides):
    return [row(name, **overrides.get(name, {})) for name in NAMES]


def document(items):
    return {"mode": "agent-e2e", "rows": items}


def run_main(report, baseline):
    with tempfile.TemporaryDirectory() as tmp:
        report_path = os.path.join(tmp, "report.json")
        baseline_path = os.path.join(tmp, "baseline.json")
        markdown_path = os.path.join(tmp, "summary.md")
        with open(report_path, "w", encoding="utf-8") as handle:
            json.dump(report, handle)
        with open(baseline_path, "w", encoding="utf-8") as handle:
            json.dump(baseline, handle)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = agent.main(["--report", report_path, "--baseline", baseline_path,
                             "--markdown", markdown_path])
        markdown = ""
        if os.path.exists(markdown_path):
            with open(markdown_path, encoding="utf-8") as handle:
                markdown = handle.read()
        return rc, out.getvalue() + err.getvalue(), markdown


class WallCeiling(unittest.TestCase):
    def test_short_journey_uses_the_absolute_floor(self):
        self.assertEqual(agent.wall_ceiling(100), 2100)

    def test_long_journey_uses_the_ratio(self):
        self.assertEqual(agent.wall_ceiling(5000), 10500)


class Comparison(unittest.TestCase):
    def test_equal_report_passes(self):
        failures, additions, notes = agent.compare(
            {item["name"]: item for item in rows()},
            {item["name"]: item for item in rows()})
        self.assertEqual(failures, [])
        self.assertEqual(additions, [])
        self.assertEqual(notes, [])

    def test_token_growth_beyond_ten_percent_fails(self):
        current = {item["name"]: item for item in rows(alpha={"prompt": 115})}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("alpha", failures[0])
        self.assertIn("tokens", failures[0])

    def test_token_growth_within_ten_percent_passes(self):
        current = {item["name"]: item for item in rows(alpha={"prompt": 109})}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(failures, [])

    def test_tool_call_growth_fails(self):
        current = {item["name"]: item for item in rows(alpha={"calls": ["read", "read"]})}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("tool calls", failures[0])

    def test_a_passed_journey_that_now_fails_is_a_regression(self):
        current = {
            item["name"]: item
            for item in rows(alpha={"ok": False, "failures": ["wrong answer"]})
        }
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("alpha", failures[0])
        self.assertIn("wrong answer", failures[0])

    def test_a_missing_journey_fails(self):
        current = {item["name"]: item for item in rows() if item["name"] != "golf"}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("golf", failures[0])
        self.assertIn("missing", failures[0])

    def test_a_new_journey_is_additive(self):
        report = rows()
        report.append(row("kilo"))
        failures, additions, _ = agent.compare(
            {item["name"]: item for item in report},
            {item["name"]: item for item in rows()})
        self.assertEqual(failures, [])
        self.assertEqual(additions, ["kilo"])

    def test_a_new_failing_journey_fails(self):
        report = rows()
        report.append(row("kilo", ok=False, failures=["boom"]))
        failures, _, _ = agent.compare(
            {item["name"]: item for item in report},
            {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("new journey", failures[0])
        self.assertIn("boom", failures[0])

    def test_wall_above_the_ceiling_fails(self):
        current = {item["name"]: item for item in rows(alpha={"wall": 3000})}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(len(failures), 1)
        self.assertIn("wall", failures[0])

    def test_wall_below_the_ceiling_passes(self):
        current = {item["name"]: item for item in rows(alpha={"wall": 2000})}
        failures, _, _ = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(failures, [])

    def test_tool_sequence_change_is_noted_not_failed(self):
        current = {item["name"]: item for item in rows(alpha={"calls": ["write"]})}
        failures, _, notes = agent.compare(
            current, {item["name"]: item for item in rows()})
        self.assertEqual(failures, [])
        self.assertTrue(any("tool sequence" in note for note in notes))


class Main(unittest.TestCase):
    def test_equal_report_passes_and_renders_the_summary(self):
        rc, output, markdown = run_main(document(rows()), document(rows()))
        self.assertEqual(rc, 0, output)
        self.assertIn("| journey | tokens |", markdown)
        self.assertIn("alpha", markdown)
        self.assertIn("pass", markdown)

    def test_below_the_journey_floor_fails_closed(self):
        rc, output, _ = run_main(document(rows()[:-1]), document(rows()))
        self.assertEqual(rc, 2)
        self.assertIn("floor", output)

    def test_a_failing_baseline_fails_closed(self):
        baseline = rows()
        baseline[0] = row(baseline[0]["name"], ok=False)
        rc, output, _ = run_main(document(rows()), document(baseline))
        self.assertEqual(rc, 2)
        self.assertIn("baseline", output)

    def test_missing_report_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline = os.path.join(tmp, "baseline.json")
            with open(baseline, "w", encoding="utf-8") as handle:
                json.dump(document(rows()), handle)
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = agent.main(["--report", os.path.join(tmp, "missing.json"),
                                 "--baseline", baseline, "--markdown", ""])
            self.assertEqual(rc, 2)
            self.assertIn("FAILED CLOSED", out.getvalue() + err.getvalue())

    def test_a_new_failing_journey_makes_the_gate_red(self):
        report = rows() + [row("kilo", ok=False, failures=["boom"])]
        rc, output, _ = run_main(document(report), document(rows()))
        self.assertEqual(rc, 1, output)
        self.assertIn("kilo", output)


if __name__ == "__main__":
    unittest.main()
