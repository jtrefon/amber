"""Tests for the wall-clock assertion lint.

The lint's job is to make a tight timing bound a reviewed exception, not a
habit. These tests pin the units, the lower/upper distinction, the marker and
the fail-closed scan floor.
"""

import contextlib
import io
import unittest

from harness import load

fl = load("test_flake_lint")


def hits(source):
    return fl.findings(source)


def quiet(fn, *args):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args)


class Detection(unittest.TestCase):
    def test_a_short_upper_bound_is_flagged(self):
        found = hits("void t() { ASSERT(elapsed.count() < 500); }\n")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][2], 500)

    def test_a_long_bound_is_clean(self):
        self.assertEqual(hits("void t() { ASSERT(elapsed.count() < 20000); }\n"), [])

    def test_the_15_second_boundary_is_clean(self):
        self.assertEqual(hits("void t() { ASSERT(elapsed.count() < 15000); }\n"), [])

    def test_seconds_are_converted(self):
        found = hits("void t() { ASSERT(waited < std::chrono::seconds(5)); }\n")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][2], 5000)

    def test_milliseconds_are_converted(self):
        found = hits("void t() { ASSERT(waited < std::chrono::milliseconds(100)); }\n")
        self.assertEqual(found[0][2], 100)

    def test_a_lower_bound_is_not_a_flake_risk(self):
        self.assertEqual(hits("void t() { ASSERT(elapsed.count() >= 900); }\n"), [])

    def test_a_number_without_a_clock_or_duration_is_not_flagged(self):
        self.assertEqual(hits("void t() { ASSERT(xs.size() < 10); }\n"), [])

    def test_a_polling_wait_is_not_flagged(self):
        self.assertEqual(hits("void t() { ASSERT_TRUE(wait_until(pred)); }\n"), [])

    def test_the_assertion_line_is_reported_across_a_multiline_statement(self):
        source = (
            "void t() {\n"
            "    const auto ms =\n"
            "        std::chrono::duration_cast<std::chrono::milliseconds>(\n"
            "            std::chrono::steady_clock::now() - t0)\n"
            "            .count();\n"
            "    ASSERT(ms < 1000);\n"
            "}\n"
        )
        found = hits(source)
        self.assertEqual(found[0][0], 6)


class Acknowledgement(unittest.TestCase):
    def test_a_reason_suppresses_the_bound(self):
        source = (
            "void t() { ASSERT(elapsed.count() < 500); "
            "// flake-allow: the regression is the 60 s deadline\n}\n"
        )
        self.assertEqual(hits(source), [])
        self.assertEqual(len(fl.acknowledged(source)), 1)

    def test_an_empty_reason_does_not_suppress(self):
        source = "void t() { ASSERT(elapsed.count() < 500); // flake-allow:\n}\n"
        self.assertEqual(len(hits(source)), 1)

    def test_the_reason_is_reported(self):
        source = (
            "void t() { ASSERT(elapsed.count() < 500); "
            "// flake-allow: one 100 ms sleep plus overhead\n}\n"
        )
        self.assertIn("one 100 ms sleep", fl.acknowledged(source)[0][2])


class GateContract(unittest.TestCase):
    def test_a_clean_scan_passes(self):
        self.assertEqual(quiet(fl.check, {}, fl.MIN_FILES_SANE), 0)

    def test_a_scan_that_read_almost_nothing_fails_closed(self):
        self.assertEqual(quiet(fl.check, {}, 0), 2)

    def test_a_finding_fails(self):
        self.assertEqual(
            quiet(fl.check, {"tests/x.cpp": [(1, "ASSERT(ms < 9);", 9)]}, fl.MIN_FILES_SANE), 1
        )

    def test_the_real_tree_is_clean(self):
        found, scanned = fl.scan()
        self.assertEqual(quiet(fl.check, found, scanned), 0)


if __name__ == "__main__":
    unittest.main()
