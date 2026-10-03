"""The debt-marker gate: a promise in a comment must be acknowledged or finished.

Every other gate measures shape; this one measures unfinished work, which is the
thing the shapes exist to prevent. It is a cliff because the tree is already at
zero, so there is no baseline to maintain.
"""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

dg = load("debt_gate")


def run_check(found, scanned=None):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = dg.check(found, dg.MIN_FILES_SANE if scanned is None else scanned)
    return rc, buf.getvalue()


class DetectsMarkers(unittest.TestCase):
    def test_clean_source(self):
        self.assertEqual(dg.debt_markers("int f() { return 1; }\n"), [])

    def test_each_marker(self):
        for marker in dg.MARKERS:
            found = dg.debt_markers(f"// {marker}: later\n")
            self.assertEqual(len(found), 1, marker)

    def test_line_numbers_are_one_indexed(self):
        found = dg.debt_markers("\n\n// TODO: x\n")
        self.assertEqual(found[0][0], 3)

    def test_the_marker_text_is_reported(self):
        found = dg.debt_markers("    // FIXME: rename\n")
        self.assertIn("FIXME", found[0][1])

    def test_several_on_one_line(self):
        self.assertEqual(len(dg.debt_markers("// TODO: a FIXME: b\n")), 1)


class WhatIsNotDebt(unittest.TestCase):
    def test_a_substring_is_not_a_marker(self):
        for text in ("int iflag;", "auto todo_list = {};", "int methodName();",
                     "int xxx_count;", "// methodical approach"):
            self.assertEqual(dg.debt_markers(text + "\n"), [], text)

    def test_a_marker_in_a_string_literal_is_still_reported(self):
        """A TODO inside a string is not a comment, but it is still a promise the
        reader sees. Flagged rather than special-cased."""
        self.assertEqual(len(dg.debt_markers('const char* s = "TODO: later";\n')), 1)

    def test_lowercase_is_not_a_marker(self):
        self.assertEqual(dg.debt_markers("// todo: later\n"), [])


class Acknowledgement(unittest.TestCase):
    def test_debt_allow_suppresses(self):
        self.assertEqual(dg.debt_markers("// TODO: x (debt-allow: tracked in #412)\n"), [])

    def test_debt_allow_needs_the_marker_to_still_be_present_to_mean_anything(self):
        self.assertEqual(len(dg.debt_markers("// something (debt-allow: why)\n")), 0)

    def test_an_empty_acknowledgement_still_suppresses(self):
        """Documented as a per-line form; policing its prose is not this gate's job."""
        self.assertEqual(dg.debt_markers("// HACK: x (debt-allow:)\n"), [])


class GateContract(unittest.TestCase):
    def test_clean_tree_passes(self):
        rc, out = run_check({})
        self.assertEqual(rc, 0, out)
        self.assertIn("clean", out)

    def test_any_marker_fails(self):
        rc, out = run_check({"lib/a.cpp": [(3, "// TODO: later")]})
        self.assertEqual(rc, 1)
        self.assertIn("lib/a.cpp:3", out)
        self.assertIn("debt-allow", out)

    def test_empty_scan_fails_closed(self):
        rc, out = run_check({}, scanned=0)
        self.assertEqual(rc, 2)
        self.assertIn("scanned", out.lower())

    def test_implausibly_small_scan_fails_closed(self):
        rc, _ = run_check({}, scanned=3)
        self.assertEqual(rc, 2)

    def test_update_refuses_to_record_debt(self):
        buf = io.StringIO()
        with redirect_stderr(buf):
            self.assertEqual(dg.update({"lib/a.cpp": [(1, "// TODO")]}), 1)
        self.assertIn("no baseline", buf.getvalue())

    def test_update_succeeds_only_when_clean(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(dg.update({}), 0)


class ScopeExcludesVendoredCode(unittest.TestCase):
    def test_nlohmann_is_not_scanned(self):
        for rel, _ in dg.source_files():
            self.assertNotIn("nlohmann", rel)
            self.assertNotIn("third_party", rel)

    def test_generated_version_header_is_not_scanned(self):
        for rel, _ in dg.source_files():
            self.assertNotIn("version.h", rel)

    def test_scan_finds_nothing_in_this_repository(self):
        """The gate is a cliff, which is only meaningful while the tree is clean."""
        self.assertEqual(dg.scan(), {}, "owned source must carry no debt markers")

    def test_the_gate_would_notice_a_marker_in_a_real_file(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False,
                                             dir=os.path.join(dg.REPO_ROOT, "lib"))
        handle.write("// TODO: this should be caught\n")
        handle.close()
        path = os.path.relpath(handle.name, dg.REPO_ROOT)
        try:
            found = dg.scan()
            self.assertIn(path, found)
            self.assertEqual(run_check({path: found[path]})[0], 1)
        finally:
            os.unlink(handle.name)
            self.assertEqual(dg.scan(), {}, "the tree must be clean again")


if __name__ == "__main__":
    unittest.main()