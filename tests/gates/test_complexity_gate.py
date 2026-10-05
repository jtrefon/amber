"""The complexity gate must fail closed.

The gate is a hard cliff with an empty baseline, so a measurement that silently
covers nothing is worse than no gate at all: it reports green forever while
measuring nothing. These tests pin the fail-closed contract that was missing.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

from harness import load

cg = load("complexity_gate")


def fn(nloc, ccn, name="f", file="lib/x.cpp", line=1, length=None, param=1):
    return {
        "file": file,
        "line": line,
        "name": name,
        "nloc": nloc,
        "ccn": ccn,
        "length": length if length is not None else nloc,
        "param": param,
    }


def clean_scan(count=None):
    """A plausible scan: enough functions, none over any cap."""
    total = cg.MIN_FUNCTIONS_SANE if count is None else count
    return [fn(10, 3, name=f"f{i}") for i in range(total)]


def run_check(functions):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cg.check(functions)
    return rc, buf.getvalue()


class FailsClosedOnNothingScanned(unittest.TestCase):
    def test_empty_scan_fails_closed(self):
        rc, out = run_check([])
        self.assertEqual(rc, 2, "an empty scan must fail closed, not pass")
        self.assertIn("measured", out.lower())

    def test_implausibly_small_scan_fails_closed(self):
        rc, _ = run_check([fn(1, 1)])
        self.assertEqual(rc, 2)

    def test_scan_at_the_sanity_floor_is_accepted(self):
        rc, out = run_check(clean_scan())
        self.assertEqual(rc, 0, out)

    def test_below_the_sanity_floor_fails_closed(self):
        rc, _ = run_check(clean_scan(cg.MIN_FUNCTIONS_SANE - 1))
        self.assertEqual(rc, 2)


class DetectsOutputFormatDrift(unittest.TestCase):
    """lizard's -w output is parsed with a regex. A lizard release that changes
    the format must be a loud failure, not an empty result set."""

    WELL_FORMED = ("lib/a.cpp:12: warning: agent::f has 5 NLOC, 2 CCN, 30 token, "
                   "1 PARAM, 9 length, 0 ND")

    def test_parses_a_well_formed_line(self):
        parsed, unparsed = cg.parse_lizard_output([self.WELL_FORMED])
        self.assertEqual(len(parsed), 1)
        self.assertEqual(unparsed, 0)
        self.assertEqual(parsed[0]["name"], "agent::f")
        self.assertEqual(parsed[0]["nloc"], 5)
        self.assertEqual(parsed[0]["ccn"], 2)
        self.assertEqual(parsed[0]["param"], 1)

    def test_unparseable_lines_are_counted_as_unparsed(self):
        parsed, unparsed = cg.parse_lizard_output(
            ["lib/a.cpp:12: warning: agent::f has 5 lines and 2 branches"])
        self.assertEqual(parsed, [])
        self.assertEqual(unparsed, 1, "a line we cannot parse must be counted, not dropped")

    def test_non_function_output_is_not_counted_as_unparsed(self):
        _, unparsed = cg.parse_lizard_output(["1 file analyzed.", "=====", "NLOC  CCN"])
        self.assertEqual(unparsed, 0)


class Exclusions(unittest.TestCase):
    """bench/scenarios/ is code the benchmark *agent* writes and an oracle scores.
    Gating it held the agent's own output to our limits, and put two benchmark
    fixtures in the CCN>=14 list."""

    def test_scenarios_are_excluded(self):
        command = cg.lizard_command()
        self.assertIn("*/scenarios/*", command)

    def test_exclude_uses_the_form_lizard_actually_matches(self):
        """A bare "bench/scenarios" matches nothing -- measured, 41 scenario lines
        still reported. Only the glob form excludes them."""
        self.assertNotIn("bench/scenarios", command_without_glob())

    def test_generated_results_are_excluded(self):
        self.assertIn("bench/results", cg.lizard_command())


def command_without_glob():
    return [a for a in cg.lizard_command() if a != "*/scenarios/*"]


class RunLizardFailsClosed(unittest.TestCase):
    def test_unknown_output_format_is_an_error(self):
        with mock.patch.object(cg, "lizard_command",
                               return_value=["python3", "-c", "print('nonsense')"]):
            functions, err = cg.run_lizard()
        self.assertIsNotNone(err, "an unrecognised lizard format must be an error")
        self.assertFalse(functions, "an error must yield no functions to check")

    def test_missing_tool_is_an_error(self):
        with mock.patch.object(cg, "lizard_command",
                               return_value=["definitely-not-installed-xyz"]):
            functions, err = cg.run_lizard()
        self.assertIsNotNone(err)
        self.assertFalse(functions)

    def test_zero_functions_from_a_real_run_is_an_error(self):
        with mock.patch.object(cg, "lizard_command",
                               return_value=["python3", "-c", "pass"]):
            functions, err = cg.run_lizard()
        self.assertIsNotNone(err)
        self.assertFalse(functions)

    def test_error_is_reported_even_when_lizard_prints_nothing(self):
        with mock.patch.object(cg, "lizard_command",
                               return_value=["python3", "-c", "raise SystemExit(3)"]):
            functions, err = cg.run_lizard()
            self.assertIsNotNone(err)
            self.assertFalse(functions)

    def test_exit_status_is_not_treated_as_failure(self):
        """lizard exits 1 on a good run: `-C 1 -L 1` makes every function a
        warning, and it ends with `if 0 <= number < warning_count: return 1`."""
        script = "import sys; print('lib/a.cpp:1: warning: f has 5 NLOC, 2 CCN, "
        script += "30 token, 1 PARAM, 9 length, 0 ND'); sys.exit(1)"
        with mock.patch.object(cg, "lizard_command",
                               return_value=["python3", "-c", script]):
            functions, err = cg.run_lizard()
        self.assertIsNone(err, "exit 1 with a parseable line is a successful scan")
        self.assertEqual(len(functions), 1)


class StillDetectsViolations(unittest.TestCase):
    def test_clean_tree_passes(self):
        rc, out = run_check(clean_scan())
        self.assertEqual(rc, 0, out)
        self.assertIn("baseline holds", out)

    def test_new_long_function_fails(self):
        functions = clean_scan() + [fn(45, 2, name="too_long")]
        rc, out = run_check(functions)
        self.assertEqual(rc, 1)
        self.assertIn("too_long", out)

    def test_new_dense_function_fails(self):
        functions = clean_scan() + [fn(12, 20, name="too_dense")]
        rc, out = run_check(functions)
        self.assertEqual(rc, 1)
        self.assertIn("too_dense", out)

    def test_new_wide_function_fails(self):
        functions = clean_scan() + [fn(5, 1, name="too_wide", param=cg.PARAM_MAX + 1)]
        rc, out = run_check(functions)
        self.assertEqual(rc, 1)
        self.assertIn("too_wide", out)


class AxesAreDeclared(unittest.TestCase):
    def test_size_axes(self):
        # Pinned so a cap change has to be a deliberate edit rather than a side effect.
        # CCN_MAX went 15 -> 14 once the four functions sitting at 15 were cleared, so the
        # cap could tighten on an empty baseline.
        self.assertEqual(cg.CCN_MAX, 14)
        self.assertEqual(cg.NLOC_MAX, 40)

    def test_no_accepted_function_exceeds_the_ccn_cap(self):
        """What makes the cap meaningful is that nothing is grandfathered under it.

        The 18 recorded functions are all over PARAM, not over CCN or NLOC -- those two
        axes have empty baselines, which is why they are cliffs rather than ratchets.
        Asserting the axis directly (rather than checking a key that may not exist) is
        what stops this passing vacuously if the baseline format changes.
        """
        import json as _json
        with open(cg.BASELINE, encoding="utf-8") as handle:
            baseline = _json.load(handle)
        over = {
            f"{path}::{name}": record.get("ccn")
            for path, entries in baseline["functions"].items()
            for name, record in entries.items()
            if record.get("ccn", 0) > cg.CCN_MAX
        }
        self.assertEqual(over, {}, f"CCN baseline is not empty: {over}")

    def test_no_accepted_function_exceeds_the_nloc_cap(self):
        import json as _json
        with open(cg.BASELINE, encoding="utf-8") as handle:
            baseline = _json.load(handle)
        over = {
            f"{path}::{name}": record.get("nloc")
            for path, entries in baseline["functions"].items()
            for name, record in entries.items()
            if record.get("nloc", 0) > cg.NLOC_MAX
        }
        self.assertEqual(over, {}, f"NLOC baseline is not empty: {over}")

    def test_the_baseline_records_the_cap_it_was_written_for(self):
        """A baseline regenerated under a different cap must not be silently reused: the
        recorded cap is what the recorded measurements were judged against."""
        import json as _json
        with open(cg.BASELINE, encoding="utf-8") as handle:
            baseline = _json.load(handle)
        self.assertEqual(baseline["ccn_max"], cg.CCN_MAX)
        self.assertEqual(baseline["nloc_max"], cg.NLOC_MAX)

    def test_parameter_cap_is_declared(self):
        self.assertGreater(cg.PARAM_MAX, 0,
                           "lizard already reports PARAM; the gate should use it")


class BaselineHandling(unittest.TestCase):
    def test_absent_baseline_is_an_error_not_a_pass(self):
        with mock.patch.object(cg, "BASELINE", "/nonexistent/complexity_baseline.json"):
            self.assertIsNone(cg.load_baseline())

    def test_empty_baseline_is_a_valid_state(self):
        """An empty baseline is the cliff this ratchet works toward, so it must be
        distinguishable from a missing file. Written to a temp path rather than
        read from the repo, because the real baseline is expected to gain entries
        as axes are added -- the contract must not depend on its contents."""
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            json.dump({"ccn_max": 15, "nloc_max": 40, "param_max": 6, "functions": {}}, fh)
            path = fh.name
        try:
            with mock.patch.object(cg, "BASELINE", path):
                self.assertEqual(cg.load_baseline(), {})
        finally:
            os.unlink(path)

    def test_populated_baseline_round_trips(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            json.dump({"functions": {"lib/a.cpp": {"f": {"nloc": 44, "ccn": 3, "param": 1}}}},
                      fh)
            path = fh.name
        try:
            with mock.patch.object(cg, "BASELINE", path):
                self.assertEqual(cg.load_baseline(),
                                 {("lib/a.cpp", "f"): {"nloc": 44, "ccn": 3, "param": 1}})
        finally:
            os.unlink(path)


class PathsAreCwdIndependent(unittest.TestCase):
    def test_baseline_resolves_from_any_directory(self):
        original = os.getcwd()
        with tempfile.TemporaryDirectory() as tmp:
            try:
                os.chdir(tmp)
                self.assertTrue(os.path.isabs(cg.BASELINE))
                self.assertTrue(os.path.isabs(cg.REPO_ROOT))
            finally:
                os.chdir(original)


class ReportNamesEveryAxis(unittest.TestCase):
    def test_report_mentions_each_axis(self):
        functions = clean_scan() + [fn(5, 1, name="too_wide", param=cg.PARAM_MAX + 1)]
        buf = io.StringIO()
        with redirect_stdout(buf):
            cg.report(functions, cg.violations_of(functions))
        out = buf.getvalue()
        for axis in ("NLOC", "CCN", "PARAM"):
            self.assertIn(axis, out)


if __name__ == "__main__":
    unittest.main()
