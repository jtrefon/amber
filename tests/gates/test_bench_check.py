"""The hermetic harness-efficiency gate.

A regression here is a harness change, not a model's mood: the fake LLM makes
tokens and tool calls deterministic. The comparator is tested with fixtures so a
real regression does not have to be committed to prove the gate works.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

bench = load("bench_check")

SCENARIOS = {
    "alpha": {"prompt_tokens": 100, "completion_tokens": 10, "tool_calls": 3,
              "wall_ms": 100, "success": True},
    "beta": {"prompt_tokens": 40, "completion_tokens": 5, "tool_calls": 1,
             "wall_ms": 50, "success": True},
}


def report_of(overrides=None, drop=None):
    scenarios = []
    for name, values in SCENARIOS.items():
        if name == drop:
            continue
        scenario = dict(values)
        scenario["name"] = name
        scenario["tool_calls_total"] = scenario.pop("tool_calls")
        scenario.update((overrides or {}).get(name, {}))
        scenarios.append(scenario)
    extra = (overrides or {}).get("_extra")
    if extra:
        scenarios.append(extra)
    return {"mode": "hermetic", "scenarios": scenarios}


def run_check(current, recorded, allow=""):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = bench.check(current, recorded, allow)
    return rc, out.getvalue() + err.getvalue()


class Comparison(unittest.TestCase):
    def current(self, overrides=None, drop=None):
        return bench.metrics(report_of(overrides, drop))

    def test_equal_report_passes(self):
        self.assertEqual(run_check(self.current(), SCENARIOS)[0], 0)

    def test_token_growth_beyond_ten_percent_fails(self):
        current = self.current({"alpha": {"prompt_tokens": 112}})
        rc, out = run_check(current, SCENARIOS)
        self.assertEqual(rc, 1)
        self.assertIn("alpha", out)
        self.assertIn("tokens", out)

    def test_token_growth_within_ten_percent_passes(self):
        current = self.current({"alpha": {"prompt_tokens": 109}})
        self.assertEqual(run_check(current, SCENARIOS)[0], 0)

    def test_extra_tool_call_fails(self):
        current = self.current({"alpha": {"tool_calls_total": 4}})
        rc, out = run_check(current, SCENARIOS)
        self.assertEqual(rc, 1)
        self.assertIn("tool calls", out)

    def test_a_ten_percent_call_increase_is_allowed(self):
        recorded = {"alpha": dict(SCENARIOS["alpha"], tool_calls=10)}
        current = {"alpha": dict(SCENARIOS["alpha"], tool_calls=11)}
        self.assertEqual(run_check(current, recorded)[0], 0)

    def test_wall_time_is_a_ceiling_not_a_percentage(self):
        # baseline 100ms -> ceiling 700ms; a 3x run fails, 6x+500 is the line.
        self.assertEqual(run_check(self.current({"alpha": {"wall_ms": 700}}), SCENARIOS)[0], 0)
        self.assertEqual(run_check(self.current({"alpha": {"wall_ms": 701}}), SCENARIOS)[0], 1)

    def test_a_success_drop_fails(self):
        current = self.current({"alpha": {"success": False}})
        rc, out = run_check(current, SCENARIOS)
        self.assertEqual(rc, 1)
        self.assertIn("success", out)

    def test_a_missing_scenario_fails_closed(self):
        """The baseline names it, so a report without it measured less than the
        gate claims. That is a measurement failure, not a small regression."""
        rc, out = run_check(self.current(drop="beta"), SCENARIOS)
        self.assertEqual(rc, 2)
        self.assertIn("beta", out)

    def test_an_additive_scenario_passes_with_a_note(self):
        current = self.current({"_extra": {
            "name": "gamma", "prompt_tokens": 1, "completion_tokens": 1,
            "tool_calls_total": 1, "wall_ms": 1, "success": True}})
        rc, out = run_check(current, SCENARIOS)
        self.assertEqual(rc, 0)
        self.assertIn("additive", out)

    def test_allow_turns_a_regression_into_a_reported_override(self):
        current = self.current({"alpha": {"prompt_tokens": 120}})
        rc, out = run_check(current, SCENARIOS, allow="known one-off")
        self.assertEqual(rc, 0)
        self.assertIn("allowed by: known one-off", out)

    def test_allow_does_not_hide_a_measurement_failure(self):
        rc, _ = run_check(self.current(drop="beta"), SCENARIOS, allow="excuse")
        self.assertEqual(rc, 2)


class Loading(unittest.TestCase):
    def write(self, payload):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(payload, handle)
        handle.close()
        return handle.name

    def test_a_report_without_scenarios_is_not_usable(self):
        self.assertIsNone(bench.metrics({"mode": "hermetic"}))
        self.assertIsNone(bench.metrics({"mode": "hermetic", "scenarios": [{}]}))

    def test_the_committed_baseline_is_hermetic_and_complete(self):
        baseline = bench.load_json(bench.BASELINE, "baseline")
        self.assertIsNotNone(baseline)
        self.assertEqual(baseline.get("mode"), "hermetic")
        recorded, error = bench.baseline_metrics(baseline)
        self.assertIsNone(error)
        self.assertGreaterEqual(len(recorded), 8)
        for name, values in recorded.items():
            for metric in bench.REQUIRED_METRICS:
                self.assertIn(metric, values, f"{name} misses {metric}")

    def test_update_round_trip(self):
        current = bench.metrics(report_of())
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        handle.close()
        try:
            bench.store(handle.name, current)
            with open(handle.name, encoding="utf-8") as handle_read:
                saved = json.load(handle_read)
        finally:
            os.unlink(handle.name)
        self.assertEqual(saved["mode"], "hermetic")
        self.assertEqual(saved["scenarios"]["alpha"]["tool_calls"], 3)

    def test_allow_can_come_from_the_environment(self):
        """CI passes the reason through the environment, not a shell recipe:
        the text is user-controlled and must never become shell syntax."""
        report = self.write(report_of({"alpha": {"prompt_tokens": 120}}))
        baseline = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        baseline.close()
        bench.store(baseline.name, SCENARIOS)
        old_allow = os.environ.get("BENCH_ALLOW")
        os.environ["BENCH_ALLOW"] = "env reason"
        import sys
        old = sys.argv
        sys.argv = ["bench_check.py", "--report", report, "--baseline", baseline.name]
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                rc = bench.main()
        finally:
            sys.argv = old
            if old_allow is None:
                os.environ.pop("BENCH_ALLOW", None)
            else:
                os.environ["BENCH_ALLOW"] = old_allow
            os.unlink(report)
            os.unlink(baseline.name)
        self.assertEqual(rc, 0)
        self.assertIn("allowed by: env reason", out.getvalue())

    def test_a_live_report_is_refused(self):
        handle = self.write({"mode": "live", "scenarios": []})
        try:
            import sys
            argv = ["bench_check.py", "--report", handle, "--update"]
            old = sys.argv
            sys.argv = argv
            try:
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    rc = bench.main()
            finally:
                sys.argv = old
        finally:
            os.unlink(handle)
        self.assertEqual(rc, 2)
        self.assertIn("hermetic", err.getvalue())


if __name__ == "__main__":
    unittest.main()
