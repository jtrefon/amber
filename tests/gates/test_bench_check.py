"""The hermetic harness-efficiency gate.

A regression here is a harness change, not a model's mood: the fake LLM makes
tokens and tool calls deterministic. The comparator is tested with fixtures so a
real regression does not have to be committed to prove the gate works.
"""

import datetime
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

bench = load("bench_check")

SCENARIOS = {
    "alpha": {"suite": "tools", "prompt_tokens": 100, "completion_tokens": 10,
              "tool_calls": 3, "wall_ms": 100, "success": True},
    "beta": {"suite": "terminal", "prompt_tokens": 40, "completion_tokens": 5,
             "tool_calls": 1, "wall_ms": 50, "success": True},
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


TODAY = datetime.date(2026, 11, 24)
EMPTY_REGISTER = {"max_age_days": 30, "cells": {}}


def run_check(current, recorded, allow="", min_scenarios=0, additive=None, today=TODAY):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = bench.check(current, recorded, allow, min_scenarios=min_scenarios, today=today,
                         additive=additive if additive is not None else EMPTY_REGISTER)
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
        # baseline 100ms -> max(2x+500, baseline+2000) = 2100ms.
        self.assertEqual(run_check(self.current({"alpha": {"wall_ms": 2100}}), SCENARIOS)[0], 0)
        self.assertEqual(run_check(self.current({"alpha": {"wall_ms": 2101}}), SCENARIOS)[0], 1)

    def test_wall_time_has_absolute_headroom_for_short_scenarios(self):
        """A 2 ms baseline must not turn one scheduling slice into a regression.

        This is the measured t-07 case: a 113 ms hermetic scenario ran at
        763-1012 ms on loaded runners, inside a 726 ms 2x ceiling's noise but
        far outside it. The absolute headroom keeps the gate a pathology check
        while the per-scenario ceiling still catches a 2x regression on a
        longer run."""
        short = {"tiny": {"suite": "tools", "prompt_tokens": 1, "completion_tokens": 1,
                          "tool_calls": 1, "wall_ms": 2, "success": True}}
        current = {"tiny": {"suite": "tools", "prompt_tokens": 1, "completion_tokens": 1,
                            "tool_calls": 0, "wall_ms": 1900, "success": True}}
        self.assertEqual(run_check(current, short)[0], 0)
        current["tiny"]["wall_ms"] = 2003
        self.assertEqual(run_check(current, short)[0], 1)

    def test_wall_time_keeps_the_ratio_for_long_scenarios(self):
        long_run = {"slow": {"suite": "tools", "prompt_tokens": 1, "completion_tokens": 1,
                             "tool_calls": 1, "wall_ms": 5000, "success": True}}
        current = dict(long_run["slow"], wall_ms=10500)
        self.assertEqual(run_check({"slow": current}, long_run)[0], 0)
        current["wall_ms"] = 10501
        self.assertEqual(run_check({"slow": current}, long_run)[0], 1)

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

    def additive(self, name="gamma", success=True):
        return self.current({"_extra": {
            "name": name, "prompt_tokens": 1, "completion_tokens": 1,
            "tool_calls_total": 1, "wall_ms": 1, "success": success}})

    def test_an_unrecorded_additive_scenario_fails(self):
        """A cell outside the baseline is a decision that has not been made:
        the register must name it, or the PR must graduate it."""
        rc, out = run_check(self.additive(), SCENARIOS)
        self.assertEqual(rc, 1)
        self.assertIn("gamma", out)
        self.assertIn("not in tests/bench_additive.json", out)

    def test_a_recorded_additive_scenario_passes_with_a_note(self):
        register = {"max_age_days": 30, "cells": {
            "gamma": {"opened": TODAY.isoformat(), "issue": "#704", "reason": "new cell"}}}
        rc, out = run_check(self.additive(), SCENARIOS, additive=register)
        self.assertEqual(rc, 0)
        self.assertIn("additive", out)

    def test_an_overdue_additive_scenario_fails(self):
        opened = (TODAY - datetime.timedelta(days=31)).isoformat()
        register = {"max_age_days": 30, "cells": {
            "gamma": {"opened": opened, "issue": "#704", "reason": "new cell"}}}
        rc, out = run_check(self.additive(), SCENARIOS, additive=register)
        self.assertEqual(rc, 1)
        self.assertIn("graduate it into the baseline", out)

    def test_a_stale_additive_entry_fails(self):
        register = {"max_age_days": 30, "cells": {
            "gamma": {"opened": TODAY.isoformat(), "issue": "#704", "reason": "gone"}}}
        rc, out = run_check(self.current(), SCENARIOS, additive=register)
        self.assertEqual(rc, 1)
        self.assertIn("remove the register entry", out)

    def test_a_waiver_does_not_hide_an_additive_policy_failure(self):
        rc, out = run_check(self.additive(), SCENARIOS,
                            allow="known until 2026-12-01")
        self.assertEqual(rc, 1)
        self.assertIn("graduation decision", out)

    def test_an_additive_scenario_that_fails_is_a_regression(self):
        register = {"max_age_days": 30, "cells": {
            "gamma": {"opened": TODAY.isoformat(), "issue": "#704", "reason": "new cell"}}}
        rc, out = run_check(self.additive(success=False), SCENARIOS, additive=register)
        self.assertEqual(rc, 1)
        self.assertIn("gamma", out)
        self.assertIn("additive scenario", out)

    def test_a_report_below_the_corpus_floor_fails_closed(self):
        rc, out = run_check(self.current(), SCENARIOS, min_scenarios=3)
        self.assertEqual(rc, 2)
        self.assertIn("corpus floor", out)

    def test_markdown_summarises_by_suite_and_harness(self):
        current = self.current()
        results = [(name, (SCENARIOS[name], current[name])) for name in sorted(SCENARIOS)]
        text = bench.markdown(results, [], [], harness={
            "harness_passed": 42, "harness_total": 42})
        self.assertIn("Harness integrity: **42/42**", text)
        self.assertIn("`tools`", text)
        self.assertIn("`terminal`", text)

    def test_markdown_shows_an_active_waiver(self):
        current = self.current()
        results = [(name, (SCENARIOS[name], current[name])) for name in sorted(SCENARIOS)]
        text = bench.markdown(results, [], [], "known until 2026-12-01", None,
                              (bench.parse_waiver("known until 2026-12-01", TODAY)[0],
                               bench.parse_waiver("known until 2026-12-01", TODAY)[1]))
        self.assertIn("Waiver: known (until 2026-12-01)", text)

    def test_zero_token_cells_are_reported_as_outcome_only(self):
        """The long-horizon policy client reports no usage. A `0` in a token
        column must not read as a measured cost of zero."""
        unmeasured = {name: dict(values, prompt_tokens=0, completion_tokens=0)
                      for name, values in SCENARIOS.items()}
        current = self.current({name: {"prompt_tokens": 0, "completion_tokens": 0}
                                for name in SCENARIOS})
        results = [(name, (unmeasured[name], current[name])) for name in sorted(SCENARIOS)]
        text = bench.markdown(results, [], [])
        self.assertIn("n/a", text)
        self.assertIn("outcome-only", text)
        self.assertNotIn("| 0 | +0.0% |", text)

    def test_usage_suddenly_appearing_is_not_silently_accepted(self):
        """An unmeasured baseline plus a measured report is a real change in
        instrumentation and must still pass through the token comparator."""
        unmeasured = {"alpha": dict(SCENARIOS["alpha"], prompt_tokens=0, completion_tokens=0)}
        current = {"alpha": dict(SCENARIOS["alpha"], prompt_tokens=100)}
        rc, out = run_check(current, unmeasured)
        self.assertEqual(rc, 1)
        self.assertIn("tokens", out)

    def test_allow_turns_a_regression_into_a_reported_override(self):
        current = self.current({"alpha": {"prompt_tokens": 120}})
        rc, out = run_check(current, SCENARIOS, allow="known one-off until 2026-12-01")
        self.assertEqual(rc, 0)
        self.assertIn("allowed by: known one-off until 2026-12-01", out)

    def test_a_waiver_without_an_expiry_fails(self):
        current = self.current({"alpha": {"prompt_tokens": 120}})
        rc, out = run_check(current, SCENARIOS, allow="known one-off")
        self.assertEqual(rc, 1)
        self.assertIn("needs an expiry", out)

    def test_an_expired_waiver_fails(self):
        current = self.current({"alpha": {"prompt_tokens": 120}})
        rc, out = run_check(current, SCENARIOS, allow="known until 2026-11-23")
        self.assertEqual(rc, 1)
        self.assertIn("expired on 2026-11-23", out)

    def test_a_waiver_beyond_the_horizon_fails(self):
        current = self.current({"alpha": {"prompt_tokens": 120}})
        rc, out = run_check(current, SCENARIOS, allow="known until 2026-12-20")
        self.assertEqual(rc, 1)
        self.assertIn("at most 14 days", out)

    def test_a_valid_waiver_with_no_regression_is_reported(self):
        rc, out = run_check(self.current(), SCENARIOS,
                            allow="known until 2026-12-01")
        self.assertEqual(rc, 0)
        self.assertIn("waiver active", out)

    def test_allow_does_not_hide_a_measurement_failure(self):
        rc, _ = run_check(self.current(drop="beta"), SCENARIOS,
                          allow="excuse until 2026-12-01")
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
        expiry = (datetime.date.today() + datetime.timedelta(days=7)).isoformat()
        os.environ["BENCH_ALLOW"] = f"env reason until {expiry}"
        import sys
        old = sys.argv
        sys.argv = ["bench_check.py", "--report", report, "--baseline", baseline.name,
                    "--min-scenarios", "0"]
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
        self.assertIn(f"allowed by: env reason until {expiry}", out.getvalue())

    def run_main(self, argv):
        import sys
        old = sys.argv
        sys.argv = ["bench_check.py"] + argv
        try:
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = bench.main()
            return rc, out.getvalue() + err.getvalue()
        finally:
            sys.argv = old

    def test_graduate_adds_the_additive_cells_without_churning_the_rest(self):
        baseline = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        baseline.close()
        report = self.write(report_of({"_extra": {
            "name": "gamma", "prompt_tokens": 1, "completion_tokens": 1,
            "tool_calls_total": 1, "wall_ms": 7, "success": True}}))
        try:
            bench.store(baseline.name, SCENARIOS)
            rc, out = self.run_main(["--report", report, "--baseline", baseline.name,
                                     "--graduate"])
            self.assertEqual(rc, 0)
            self.assertIn("graduated 1 additive cell", out)
            with open(baseline.name, encoding="utf-8") as handle:
                saved = json.load(handle)["scenarios"]
            self.assertIn("gamma", saved)
            self.assertEqual(saved["gamma"]["wall_ms"], 7)
            self.assertEqual(saved["alpha"], SCENARIOS["alpha"])
        finally:
            os.unlink(report)
            os.unlink(baseline.name)

    def test_graduate_with_no_additive_cell_is_a_no_op(self):
        baseline = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        baseline.close()
        report = self.write(report_of())
        try:
            bench.store(baseline.name, SCENARIOS)
            before = open(baseline.name, encoding="utf-8").read()
            rc, out = self.run_main(["--report", report, "--baseline", baseline.name,
                                     "--graduate"])
            self.assertEqual(rc, 0)
            self.assertIn("no additive cells", out)
            self.assertEqual(before, open(baseline.name, encoding="utf-8").read())
        finally:
            os.unlink(report)
            os.unlink(baseline.name)

    def test_the_committed_additive_register_is_usable(self):
        register = bench.load_additive(bench.ADDITIVE_REGISTER)
        self.assertIsNotNone(register)
        self.assertIsInstance(register.get("cells"), dict)
        self.assertGreaterEqual(register.get("max_age_days", 0), 1)

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
