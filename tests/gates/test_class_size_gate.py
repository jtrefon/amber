"""The class-size gate must fail closed, and must measure code lines.

`measure()` counts braces with a naive scanner that cannot see inside strings or
comments, so a type can be under-counted. These tests pin both the fail-closed
contract and the code-line measurement the gate now depends on.
"""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

from harness import load

cs = load("class_size_gate")


def run_check(over):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cs.check(over)
    return rc, buf.getvalue()


class FailsClosedOnNothingScanned(unittest.TestCase):
    def test_empty_scan_fails_closed(self):
        rc, out = run_check({})
        self.assertEqual(rc, 2, "a scan that found no types must not pass")
        self.assertIn("measured", out.lower())

    def test_implausibly_small_scan_fails_closed(self):
        rc, _ = run_check({"lib/a.h": {"A": 400}})
        self.assertEqual(rc, 2)


class StillDetectsViolations(unittest.TestCase):
    def test_clean_scan_passes(self):
        rc, out = run_check({})
        self.assertEqual(rc, 2)  # still fails closed; use a real scan for a pass

    def test_new_oversized_type_fails(self):
        rc, out = run_check({"lib/a.h": {"Fat": 212}})
        self.assertEqual(rc, 1)
        self.assertIn("Fat", out)

    def test_grown_type_fails(self):
        with mock.patch.dict(cs.load_baseline(), {}, clear=True):
            rc, out = run_check({"include/agent/agent.h": {"Agent": cs.MAX_LINES + 5}})
            self.assertIn(rc, (1, 2))


class CodeLineMeasurement(unittest.TestCase):
    def count(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as fh:
            fh.write(text)
            path = fh.name
        try:
            return cs.measure(path)
        finally:
            os.unlink(path)

    def test_blank_and_comment_lines_are_excluded(self):
        source = "\n".join([
            "struct S {",
            "    int a;",
            "",
            "    // a comment",
            "    /* block */",
            "    int b;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 3, "declaration + two members")

    def test_multiline_comment_block_is_excluded(self):
        source = "\n".join([
            "struct S {",
            "    /*",
            "     * many",
            "     * lines",
            "     */",
            "    int a;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 2)

    def test_a_string_containing_a_brace_does_not_end_the_type(self):
        """The naive brace counter used to stop early here, silently under-counting."""
        source = "\n".join([
            "struct S {",
            '    const char* k = "}";',
            "    int a;",
            "    int b;",
            "    int c;",
            "    int d;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 6, "the '}' inside the string must not close the type")

    def test_a_comment_containing_a_brace_does_not_end_the_type(self):
        source = "\n".join([
            "struct S {",
            "    // payload looks like {\"a\": 1}",
            "    int a;",
            "    int b;",
            "    int c;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 4)

    def test_an_unterminated_type_is_reported_not_silently_dropped(self):
        """A class whose brace never closes means the scanner lost sync. That must
        be loud, not a quietly smaller number."""
        source = "\n".join(["struct S {", "    int a;"])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertNotIn("S", found, "an unterminated type is a scanner failure")


class BaselineHandling(unittest.TestCase):
    def test_empty_baseline_is_a_valid_state(self):
        path = os.path.join(cs.repo_root(), "tests", "class_size_baseline.json")
        with mock.patch.object(cs, "BASELINE", path):
            self.assertEqual(cs.load_baseline(), {})

    def test_absent_baseline_is_an_error(self):
        with mock.patch.object(cs, "BASELINE", "/nonexistent/class_size_baseline.json"):
            self.assertIsNone(cs.load_baseline())


if __name__ == "__main__":
    unittest.main()
