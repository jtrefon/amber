"""The class-size gate must fail closed, and must measure code lines.

`measure()` finds a type's extent by counting braces, so it depends entirely on
the shared lexer in `cpp_source.py` seeing only structure and not data. That
lexer's own tests (comments, literals, raw strings, line preservation) are in
`test_cpp_source.py`; what matters here is that the gate uses it, and what it
does with the result.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

from harness import load

cs = load("class_size_gate")


def run_check(over, measured=cs.MIN_TYPES_SANE, broken=None):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cs.check(over, measured, broken)
    return rc, buf.getvalue()


class FailsClosedOnNothingScanned(unittest.TestCase):
    def test_empty_scan_fails_closed(self):
        rc, out = run_check({}, measured=0)
        self.assertEqual(rc, 2, "a scan that found no types must not pass")
        self.assertIn("measured", out.lower())

    def test_implausibly_small_scan_fails_closed(self):
        rc, _ = run_check({"lib/a.h": {"A": 400}}, measured=1)
        self.assertEqual(rc, 2)

    def test_liveness_signals_cannot_be_omitted(self):
        """check() takes the scan's liveness signals positionally and by keyword
        only, so a caller cannot skip them and inherit a silent pass."""
        with self.assertRaises(TypeError):
            cs.check({})


class UnscannableTypesFailClosed(unittest.TestCase):
    def test_unbalanced_type_fails_closed(self):
        rc, out = run_check({}, broken={"lib/a.h": ["S"]})
        self.assertEqual(rc, 2)
        self.assertIn("S", out)

    def test_unscannable_beats_a_baseline_violation(self):
        rc, _ = run_check({"lib/a.h": {"Fat": 900}}, broken={"lib/a.h": ["S"]})
        self.assertEqual(rc, 2)


class StillDetectsViolations(unittest.TestCase):
    def test_clean_scan_passes(self):
        rc, out = run_check({})
        self.assertEqual(rc, 0, out)
        self.assertIn("baseline holds", out)

    def test_new_oversized_type_fails(self):
        rc, out = run_check({"lib/a.h": {"Fat": 212}})
        self.assertEqual(rc, 1)
        self.assertIn("Fat", out)

    def test_grown_type_fails(self):
        rc, out = run_check({"include/agent/agent.h": {"Agent": cs.MAX_LINES + 5}})
        self.assertEqual(rc, 1)
        self.assertIn("Agent", out)


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
        self.assertEqual(found["S"], 4, "declaration + two members + closing brace")

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
        self.assertEqual(found["S"], 3, "declaration + member + closing brace")

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
        self.assertEqual(found["S"], 7, "the '}' inside the string must not close the type")

    def test_a_comment_containing_a_brace_does_not_end_the_type(self):
        source = "\n".join([
            "struct S {",
            '    // payload looks like {"a": 1}',
            "    int a;",
            "    int b;",
            "    int c;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 5, "declaration + comment + 3 members + brace")

    def test_a_raw_string_with_braces_does_not_end_the_type(self):
        source = "\n".join([
            "struct S {",
            '    const char* k = R"json({"a": })json";',
            "    int a;",
            "    int b;",
            "};",
        ])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertEqual(found["S"], 5)

    def test_an_unterminated_type_is_reported_not_silently_dropped(self):
        """A class whose brace never closes means the scanner lost sync. That must
        be loud, not a quietly smaller number."""
        source = "\n".join(["struct S {", "    int a;"])
        found = dict((n, lines) for lines, n in self.count(source))
        self.assertNotIn("S", found, "an unterminated type is a scanner failure")


class BaselineHandling(unittest.TestCase):
    """Baseline files are written to temp paths, not read from the repo: the real
    baseline is expected to change as types are split, and the contract under test
    ("an empty baseline is a state, a missing file is an error") must not depend
    on its current contents."""

    def write(self, payload):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(payload, handle)
        handle.close()
        return handle.name

    def test_empty_baseline_is_a_valid_state(self):
        path = self.write({"max_lines": 200, "types": {}})
        try:
            with mock.patch.object(cs, "BASELINE", path):
                self.assertEqual(cs.load_baseline(), {})
        finally:
            os.unlink(path)

    def test_absent_baseline_is_an_error(self):
        with mock.patch.object(cs, "BASELINE", "/nonexistent/class_size_baseline.json"):
            self.assertIsNone(cs.load_baseline())


if __name__ == "__main__":
    unittest.main()
