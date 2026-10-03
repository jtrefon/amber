"""The lint ratchet must fail closed, and must never baseline a hard error.

clang-tidy exits non-zero for ordinary findings, so its exit status says nothing
about whether it ran. Worse, a compile error surfaces as a `[clang-diagnostic-error]`
diagnostic, which looks enough like a finding that `--update` would happily record
it as "known" — and then the tree could stop compiling while the gate stayed green.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

from harness import load

lb = load("lint_baseline")

def empty_baseline_file():
    """A temp baseline holding `{"files": {}}`.

    The real baseline is expected to change as findings are fixed, so the
    contract under test must not depend on its current contents.
    """
    handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump({"files": {}}, handle)
    handle.close()
    return handle.name


def run_check(counts, **kwargs):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = lb.check(counts, **kwargs)
    return rc, buf.getvalue()


def finding(path, check):
    return {path: {check: 1}}


class HardErrorsAreNeverFindings(unittest.TestCase):
    def test_compile_error_is_not_counted_as_a_finding(self):
        text = "lib/a.cpp:1:10: error: use of undeclared identifier 'x' " \
               "[clang-diagnostic-error]"
        self.assertEqual(lb.parse(text), {}, "a compile error must not enter the baseline")

    def test_error_while_processing_is_not_a_finding(self):
        text = "Error while processing /tmp/a.cpp."
        self.assertEqual(lb.parse(text), {})

    def test_real_warning_is_still_counted(self):
        text = "lib/a.cpp:3:5: warning: local variable 'x' is unused " \
               "[misc-unused-variable]"
        self.assertEqual(lb.parse(text), {"lib/a.cpp": {"misc-unused-variable": 1}})

    def test_hard_errors_fail_the_gate_closed(self):
        counts = {"lib/a.cpp": {"clang-diagnostic-error": 1}}
        rc, out = run_check(counts)
        self.assertEqual(rc, 2, "a hard clang-tidy error must fail closed")
        self.assertIn("clang-diagnostic-error", out)

    def test_error_while_processing_fails_the_gate_closed(self):
        rc, out = run_check({}, errored=True)
        self.assertEqual(rc, 2)
        self.assertIn("clang-tidy", out)


class LivenessIsVerified(unittest.TestCase):
    """Zero findings is a legitimate state, so it cannot be distinguished from a
    crashed run by counting alone. The caller states how many translation units it
    asked clang-tidy to process; a shortfall means the gate measured nothing."""

    def test_matching_tu_count_passes(self):
        rc, out = run_check({}, ran=120, expected=120)
        self.assertEqual(rc, 0, out)

    def test_fewer_tus_than_expected_fails_closed(self):
        rc, out = run_check({}, ran=3, expected=120)
        self.assertEqual(rc, 2)
        self.assertIn("clang-tidy", out)

    def test_zero_tus_fails_closed(self):
        rc, out = run_check({}, ran=0, expected=120)
        self.assertEqual(rc, 2)

    def test_no_expectation_means_no_liveness_requirement(self):
        rc, out = run_check({})
        self.assertEqual(rc, 0, out)


class RatchetBehaviour(unittest.TestCase):
    def test_new_finding_fails(self):
        rc, out = run_check(finding("lib/a.cpp", "misc-unused-variable"))
        self.assertEqual(rc, 1)
        self.assertIn("misc-unused-variable", out)

    def test_grown_count_fails(self):
        rc, out = run_check({"lib/a.cpp": {"misc-unused-variable": 3}})
        self.assertEqual(rc, 1)

    def test_absent_baseline_is_an_error(self):
        with mock.patch.object(lb, "BASELINE", "/nonexistent/lint_baseline.json"):
            rc, out = run_check({})
            self.assertEqual(rc, 2)

    def test_empty_baseline_is_a_valid_state(self):
        path = empty_baseline_file()
        try:
            with mock.patch.object(lb, "BASELINE", path):
                self.assertEqual(lb.load_baseline(), {})
        finally:
            os.unlink(path)

    def test_update_writes_only_real_findings(self):
        counts = {"lib/a.cpp": {"misc-unused-variable": 1}}
        path = empty_baseline_file()
        try:
            with mock.patch.object(lb, "BASELINE", path), redirect_stdout(io.StringIO()):
                self.assertEqual(lb.update(counts), 0)
                self.assertEqual(lb.load_baseline(), counts)
        finally:
            os.unlink(path)

    def test_update_refuses_to_record_a_compile_error(self):
        counts = {"lib/a.cpp": {lb.HARD_ERROR: 1}}
        path = empty_baseline_file()
        try:
            with mock.patch.object(lb, "BASELINE", path), redirect_stderr(io.StringIO()):
                self.assertEqual(lb.update(counts, hard_errors=1), 2)
                self.assertEqual(lb.load_baseline(), {},
                                 "a tree that does not compile must not become the baseline")
        finally:
            os.unlink(path)


class PathNormalisation(unittest.TestCase):
    def test_findings_are_recorded_relative_to_the_repo(self):
        text = f"{lb.repo_root()}/lib/a.cpp:1:1: warning: x [misc-unused-variable]"
        self.assertEqual(lb.parse(text), {"lib/a.cpp": {"misc-unused-variable": 1}})


if __name__ == "__main__":
    unittest.main()
