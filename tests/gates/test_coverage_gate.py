"""Patch coverage: the documented "new code paths must have >=80% line coverage".

The whole-core 80% floor is already enforced by `gcovr --fail-under-line` in CI,
but that floor is a *project* number: it stays satisfied while new, untested code
is added, as long as the total does not move. The rule in
PULL_REQUEST_TEMPLATE.md is about the *diff*, so it needs the diff.

It also cannot be delegated to Codecov here: the upload is rejected by Codecov
("Repository not found" -- the Codecov GitHub App is not installed on this repo)
and `fail_ci_if_error: false` hides it, so `.codecov.yml`'s patch target has never
been evaluated. Measuring it in-repo makes the rule real and keeps it working with
no third-party service in the path.
"""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from harness import load

cg = load("coverage_gate")


def run_check(result, changed):
    """`check` prints its verdict and returns a code, like every other gate."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cg.check(result, changed)
    return rc, buf.getvalue()

def hits(files):
    """{path: {lineno: hits}} -- the shape `read_report` produces."""
    return {path: dict(lines) for path, lines in files.items()}


class PatchCoverageMath(unittest.TestCase):
    def test_no_changed_lines_is_not_a_violation(self):
        """A docs-only or whitespace-only change has no lines to cover. It must
        pass, not divide by zero."""
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), {})
        self.assertEqual(result.total, 0)
        self.assertIsNone(result.percent)
        self.assertTrue(result.passes)

    def test_all_changed_lines_covered(self):
        report = hits({"lib/a.cpp": [(10, 1), (11, 4), (12, 0)]})
        result = cg.patch_coverage(report, {"lib/a.cpp": {10, 11, 12}})
        self.assertEqual(result.total, 3)
        self.assertEqual(result.covered, 2)
        self.assertAlmostEqual(result.percent, 200 / 3)

    def test_nothing_covered_fails(self):
        report = hits({"lib/a.cpp": {10: 0, 11: 0}})
        result = cg.patch_coverage(report, {"lib/a.cpp": {10, 11}})
        self.assertEqual(result.percent, 0.0)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 10), ("lib/a.cpp", 11)])

    def test_only_changed_lines_count(self):
        """An untested line the PR did not touch must not drag the patch number
        down -- that is what the whole-core floor is for."""
        report = hits({"lib/a.cpp": [(10, 0), (11, 1), (12, 1)]})
        result = cg.patch_coverage(report, {"lib/a.cpp": {11, 12}})
        self.assertEqual(result.total, 2)
        self.assertEqual(result.percent, 100.0)

    def test_multiple_files_are_pooled(self):
        report = hits({"lib/a.cpp": {10: 1}, "lib/b.cpp": {10: 0, 11: 0}})
        result = cg.patch_coverage(report, {"lib/a.cpp": {10}, "lib/b.cpp": {10, 11}})
        self.assertEqual(result.total, 3)
        self.assertEqual(result.covered, 1)

    def test_a_changed_line_with_no_coverage_record_is_untested(self):
        """A line the diff touches that gcovr never reported cannot be assumed
        covered. This is the fail-open direction, so it counts as uncovered."""
        report = hits({"lib/a.cpp": {10: 1}})
        result = cg.patch_coverage(report, {"lib/a.cpp": {10, 99}})
        self.assertEqual(result.total, 2)
        self.assertEqual(result.covered, 1)
        self.assertIn(("lib/a.cpp", 99), result.uncovered)


class OnlyInstrumentedCodeCounts(unittest.TestCase):
    """A PR that edits ci.yml, a prompt or a Markdown file adds lines no test can
    execute. Counting them would fail the gate on documentation -- the fastest way
    to get a gate switched off."""

    def test_yaml_changes_are_ignored(self):
        report = hits({"lib/a.cpp": {10: 0}})
        changed = {".github/workflows/ci.yml": {1, 2, 3, 4},
                   "prompts/system.md": {1, 2}, "AGENTS.md": {5}}
        result = cg.patch_coverage(report, changed)
        self.assertEqual(result.total, 0)
        self.assertTrue(result.passes)

    def test_c_and_cpp_count(self):
        report = hits({"lib/a.cpp": {10: 0}, "lib/b.c": {3: 0}, "include/x.h": {7: 0}})
        changed = {"lib/a.cpp": {10}, "lib/b.c": {3}, "include/x.h": {7}}
        self.assertEqual(cg.patch_coverage(report, changed).total, 3)

    def test_python_and_shell_are_ignored(self):
        report = hits({"tools/x.py": {1: 0}, "tools/y.sh": {1: 0}})
        changed = {"tools/x.py": {1}, "tools/y.sh": {1}}
        self.assertEqual(cg.patch_coverage(report, changed).total, 0)

    def test_ignored_paths_are_reported_not_silent(self):
        skipped = cg.skipped_paths({".github/workflows/ci.yml": {1}, "lib/a.cpp": {2}})
        self.assertEqual(skipped, [".github/workflows/ci.yml"])

    def test_instrumented_predicate(self):
        for path in ("lib/a.cpp", "lib/b.c", "include/c.hpp"):
            self.assertTrue(cg.is_instrumented(path), path)
        for path in ("ci.yml", "a.md", "b.py", "Makefile.in"):
            self.assertFalse(cg.is_instrumented(path), path)


class Threshold(unittest.TestCase):
    def test_threshold_is_eighty(self):
        self.assertEqual(cg.COVERAGE_MIN, 80)

    def test_gate_fails_below_threshold(self):
        report = hits({"lib/a.cpp": [(n, 0 if n < 8 else 1) for n in range(1, 11)]})
        changed = {"lib/a.cpp": set(range(1, 11))}
        rc, out = run_check(cg.patch_coverage(report, changed), changed)
        self.assertEqual(rc, 1)
        self.assertIn("80", out)

    def test_gate_passes_at_threshold(self):
        report = hits({"lib/a.cpp": [(n, 1 if n < 9 else 0) for n in range(1, 11)]})
        changed = {"lib/a.cpp": set(range(1, 11))}
        rc, _ = run_check(cg.patch_coverage(report, changed), changed)
        self.assertEqual(rc, 0, "exactly 80% must pass; the bar is >= not >")

    def test_gate_passes_when_nothing_changed(self):
        rc, out = run_check(cg.patch_coverage(hits({"lib/a.cpp": {10: 0}}), {}), {})
        self.assertEqual(rc, 0, out)
        self.assertIn("no changed", out.lower())


class FailClosed(unittest.TestCase):
    def test_missing_report_file_fails_closed(self):
        rc, out = run_check(cg.patch_coverage(None, {"lib/a.cpp": {10}}), {"lib/a.cpp": {10}})
        self.assertEqual(rc, 2)
        self.assertIn("report", out.lower())

    def test_empty_report_fails_closed(self):
        rc, out = run_check(cg.patch_coverage({"files": []}, {"lib/a.cpp": {10}}),
                           {"lib/a.cpp": {10}})
        self.assertEqual(rc, 2)

    def test_no_diff_fails_closed(self):
        """No changed-line set means we cannot tell covered from uncovered, and
        reporting 100% would be a lie."""
        rc, out = run_check(cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), None), None)
        self.assertEqual(rc, 2)
        self.assertIn("diff", out.lower())

    def test_empty_mapping_fails_closed(self):
        rc, _ = run_check(cg.patch_coverage(hits({}), {"lib/a.cpp": {10}}),
                          {"lib/a.cpp": {10}})
        self.assertEqual(rc, 2)


COBERTURA = """<?xml version="1.0" ?>
<coverage line-rate="0.5">
  <packages><package><classes>
    <class filename="lib/a.cpp" name="a_cpp">
      <lines><line number="10" hits="1"/><line number="11" hits="0"/></lines>
    </class>
    <class filename="lib/b.cpp" name="b_cpp">
      <lines><line number="5" hits="3"/></lines>
    </class>
  </classes></package></packages>
</coverage>
"""


class ReadingTheReport(unittest.TestCase):
    def write(self, text):
        handle = tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False)
        handle.write(text)
        handle.close()
        return handle.name

    def test_reads_cobertura_per_line_hits(self):
        path = self.write(COBERTURA)
        try:
            loaded = cg.read_report(path)
            self.assertEqual(loaded["lib/a.cpp"], {10: 1, 11: 0})
            self.assertEqual(loaded["lib/b.cpp"], {5: 3})
        finally:
            os.unlink(path)

    def test_missing_file_is_reported_not_raised(self):
        self.assertIsNone(cg.read_report("/nonexistent/coverage.xml"))

    def test_malformed_xml_is_reported_not_raised(self):
        path = self.write("this is not xml at all")
        try:
            self.assertIsNone(cg.read_report(path))
        finally:
            os.unlink(path)

    def test_coverage_of_a_line_from_a_real_report_shape(self):
        """End to end over the fixture: added lines 10 and 11 -> 50%."""
        path = self.write(COBERTURA)
        try:
            result = cg.patch_coverage(cg.read_report(path), {"lib/a.cpp": {10, 11}})
        finally:
            os.unlink(path)
        self.assertEqual(result.total, 2)
        self.assertEqual(result.covered, 1)
        self.assertFalse(result.passes)


class ChangedLines(unittest.TestCase):
    def test_parses_a_unified_diff(self):
        diff = [
            "diff --git a/lib/a.cpp b/lib/a.cpp",
            "--- a/lib/a.cpp",
            "+++ b/lib/a.cpp",
            "@@ -10,3 +10,4 @@ void f() {",
            " unchanged",
            "+added one",
            "+added two",
            " context",
            "-removed",
        ]
        changed = cg.changed_lines_from_diff(diff)
        self.assertEqual(changed["lib/a.cpp"], {11, 12})

    def test_merges_multiple_hunks_in_one_file(self):
        diff = [
            "+++ b/lib/a.cpp",
            "@@ -1,2 +1,3 @@",
            "+one",
            "@@ -20,2 +21,3 @@",
            "+two",
        ]
        # "@@ -1,2 +1,3 @@" puts the hunk's first line at 1, so the first added
        # line IS line 1; "+21,3" puts the second hunk's first added line at 21.
        self.assertEqual(cg.changed_lines_from_diff(diff)["lib/a.cpp"], {1, 21})

    def test_ignores_files_with_no_added_lines(self):
        diff = ["+++ b/lib/a.cpp", "@@ -1,2 +1,2 @@", " ctx", "-gone"]
        self.assertNotIn("lib/a.cpp", cg.changed_lines_from_diff(diff))

    def test_ignores_dev_null_targets(self):
        self.assertEqual(cg.changed_lines_from_diff(["+++ /dev/null"]), {})

    def test_new_file_counts_all_its_lines(self):
        diff = ["--- /dev/null", "+++ b/lib/new.cpp", "@@ -0,0 +1,3 @@", "+a", "+b", "+c"]
        self.assertEqual(cg.changed_lines_from_diff(diff)["lib/new.cpp"], {1, 2, 3})

    def test_binary_files_are_skipped(self):
        diff = ["+++ b/logo.png", "Binary files /dev/null and b/logo.png differ"]
        self.assertEqual(cg.changed_lines_from_diff(diff), {})


if __name__ == "__main__":
    unittest.main()