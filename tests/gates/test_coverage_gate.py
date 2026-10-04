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


def chg(*specs):
    """Build the `changed` shape: {path: {lineno: text}}.

    Text defaults to a placeholder that will not match any base-revision line, so
    a line counts as genuinely new unless a test supplies text for carryover.
    """
    out = {}
    for path, numbers in specs:
        out.setdefault(path, {})
        for n in numbers:
            out[path][n] = f"    statement_{n}"
    return out


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
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [10, 11, 12])))
        self.assertEqual(result.total, 3)
        self.assertEqual(result.covered, 2)
        self.assertAlmostEqual(result.percent, 200 / 3)

    def test_nothing_covered_fails(self):
        report = hits({"lib/a.cpp": {10: 0, 11: 0}})
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [10, 11])))
        self.assertEqual(result.percent, 0.0)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 10), ("lib/a.cpp", 11)])

    def test_only_changed_lines_count(self):
        """An untested line the PR did not touch must not drag the patch number
        down -- that is what the whole-core floor is for."""
        report = hits({"lib/a.cpp": [(10, 0), (11, 1), (12, 1)]})
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [11, 12])))
        self.assertEqual(result.total, 2)
        self.assertEqual(result.percent, 100.0)

    def test_multiple_files_are_pooled(self):
        report = hits({"lib/a.cpp": {10: 1}, "lib/b.cpp": {10: 0, 11: 0}})
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [10]), ("lib/b.cpp", [10, 11])))
        self.assertEqual(result.total, 3)
        self.assertEqual(result.covered, 1)

    def test_a_changed_line_with_no_coverage_record_is_not_executable(self):
        """gcovr's Cobertura output omits comment, blank and brace lines entirely,
        while an executable line that never ran is present with hits=0. So an
        absent line is NOT an untested line.

        I had this backwards, and it failed a PR whose whole addition to a .cpp was
        four lines of explanatory comment -- demanding that a comment be executed.
        """
        report = hits({"lib/a.cpp": {10: 1, 11: 0}})
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [10, 11, 97, 98, 99])))
        self.assertEqual(result.total, 2, "only the two executable lines count")
        self.assertEqual(result.covered, 1)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 11)])

    def test_a_comment_only_change_is_not_a_violation(self):
        report = hits({"lib/a.cpp": {10: 1, 11: 0}})
        result = cg.patch_coverage(report, chg(("lib/a.cpp", [20, 21, 22, 23])))
        self.assertEqual(result.total, 0)
        self.assertTrue(result.passes)


class UnmeasuredFilesFailClosed(unittest.TestCase):
    """A changed .cpp absent from the report entirely was compiled, so it must have
    been measured. That is a gap in the measurement, not a low score -- the opposite
    conclusion from an absent *line*."""

    def test_a_compiled_file_missing_from_the_report_fails_closed(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), chg(("lib/new.cpp", [1, 2])))
        self.assertEqual(result.unmeasured, ["lib/new.cpp"])
        rc, out = run_check(result, chg(("lib/new.cpp", [1, 2])))
        self.assertEqual(rc, 2)
        self.assertIn("lib/new.cpp", out)

    def test_an_absent_header_is_not_a_failure(self):
        """A header with no executable lines of its own is routinely absent from
        the report. Failing on that would block every header-touching PR."""
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), chg(("include/agent/x.h", [5, 6])))
        self.assertEqual(result.unmeasured, [])
        self.assertEqual(result.total, 0)
        self.assertEqual(run_check(result, chg(("include/agent/x.h", [5, 6])))[0], 0)


class DeliberatelyNotMeasured(unittest.TestCase):
    """The coverage report is built with gcovr --exclude, which omits the main()
    entry points on purpose. That is the same category as tests/: a changed source
    the report intends to omit is NOT a measurement gap.

    The distinction is load-bearing, because the default for an absent .cpp is to
    fail closed. Conflating "excluded on purpose" with "compiled but not measured"
    blocked a PR that only refactored bench/main.cpp, for the wrong reason.
    """

    ENTRY_POINTS = (r".*src/main\.cpp", r".*bench/main\.cpp", r".*tui/tui_main\.cpp")

    def test_a_gcovr_excluded_entry_point_is_not_unmeasured(self):
        for path in ("src/main.cpp", "bench/main.cpp", "tui/tui_main.cpp"):
            with self.subTest(path=path):
                result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}),
                                           chg((path, [1, 2])), {}, self.ENTRY_POINTS)
                self.assertEqual(result.unmeasured, [])
                self.assertEqual(run_check(result, chg((path, [1, 2])))[0], 0)

    def test_an_excluded_entry_point_still_fails_without_the_pattern(self):
        """The exemption is opt-in and passed by the workflow, so the fail-closed
        default is unchanged for anything the patterns do not name."""
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), chg(("bench/main.cpp", [1, 2])))
        self.assertEqual(result.unmeasured, ["bench/main.cpp"])
        self.assertEqual(run_check(result, chg(("bench/main.cpp", [1, 2])))[0], 2)

    def test_the_patterns_do_not_widen_to_other_bench_sources(self):
        """bench/main.cpp is excluded; the rest of bench/ is measured. A pattern loose
        enough to cover both would silently exempt measured code."""
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}),
                                   chg(("bench/oracle.cpp", [1, 2])), {}, self.ENTRY_POINTS)
        self.assertEqual(result.unmeasured, ["bench/oracle.cpp"])

    def test_a_tests_path_is_still_excluded_without_any_pattern(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), chg(("tests/run_tests.cpp", [1])))
        self.assertEqual(result.unmeasured, [])
        self.assertEqual(result.total, 0)

    def test_deliberately_not_measured_predicate(self):
        self.assertTrue(cg.deliberately_not_measured("tests/x.cpp"))
        self.assertTrue(cg.deliberately_not_measured("bench/main.cpp", (r".*bench/main\.cpp",)))
        self.assertFalse(cg.deliberately_not_measured("bench/oracle.cpp", (r".*bench/main\.cpp",)))
        self.assertFalse(cg.deliberately_not_measured("lib/a.cpp"))


class OnlyInstrumentedCodeCounts(unittest.TestCase):
    """A PR that edits ci.yml, a prompt or a Markdown file adds lines no test can
    execute. Counting them would fail the gate on documentation -- the fastest way
    to get a gate switched off."""

    def test_yaml_changes_are_ignored(self):
        report = hits({"lib/a.cpp": {10: 0}})
        changed = chg((".github/workflows/ci.yml", [1, 2, 3, 4]),
                    ("prompts/system.md", [1, 2]), ("AGENTS.md", [5]))
        result = cg.patch_coverage(report, changed)
        self.assertEqual(result.total, 0)
        self.assertTrue(result.passes)

    def test_c_and_cpp_count(self):
        report = hits({"lib/a.cpp": {10: 0}, "lib/b.c": {3: 0}, "include/x.h": {7: 0}})
        changed = chg(("lib/a.cpp", [10]), ("lib/b.c", [3]), ("include/x.h", [7]))
        self.assertEqual(cg.patch_coverage(report, changed).total, 3)

    def test_test_files_are_ignored(self):
        """The coverage report excludes tests/ on purpose -- you do not measure
        coverage of the thing that measures coverage -- so a PR that only adds a
        test would otherwise fail this gate with "absent from the report"."""
        report = hits({"lib/a.cpp": {10: 0}})
        changed = chg(("tests/run_tests.cpp", [1, 2, 3]), ("tests/gates/test_x.py", [1]))
        result = cg.patch_coverage(report, changed)
        self.assertEqual(result.total, 0)
        self.assertTrue(result.passes)
        self.assertEqual(run_check(result, changed)[0], 0)

    def test_python_and_shell_are_ignored(self):
        report = hits({"tools/x.py": {1: 0}, "tools/y.sh": {1: 0}})
        changed = chg(("tools/x.py", [1]), ("tools/y.sh", [1]))
        self.assertEqual(cg.patch_coverage(report, changed).total, 0)

    def test_ignored_paths_are_reported_not_silent(self):
        skipped = cg.skipped_paths({".github/workflows/ci.yml": {1}, "lib/a.cpp": {2}})
        self.assertEqual(skipped, [".github/workflows/ci.yml"])

    def test_instrumented_predicate(self):
        for path in ("lib/a.cpp", "lib/b.c", "include/c.hpp"):
            self.assertTrue(cg.is_instrumented(path), path)
        for path in ("ci.yml", "a.md", "b.py", "Makefile.in"):
            self.assertFalse(cg.is_instrumented(path), path)


class Carryover(unittest.TestCase):
    """A line the diff moved is not new code. The gate's first live run failed a
    pure refactor because every line lifted out of an inline lambda was reported
    as uncovered -- so a gate that punishes code motion blocks exactly the
    refactoring this project wants to encourage."""

    def test_a_moved_uncovered_line_is_not_held_against_the_patch(self):
        """The shape of a real move: a block lifted verbatim from elsewhere in the
        same file, so the text at the new location already existed at the base."""
        report = hits({"lib/a.cpp": {40: 0, 41: 0, 42: 0}})
        changed = {"lib/a.cpp": {40: "    a();", 41: "    b();", 42: "    c();"}}
        carried = {"lib/a.cpp": {cg.normalise(t) for t in ("a();", "b();", "c();")}}
        result = cg.patch_coverage(report, changed, carried)
        self.assertEqual(result.total, 0)
        self.assertEqual(result.carried, 3)
        self.assertTrue(result.passes)

    def test_a_partly_moved_block_still_fails_on_what_is_new(self):
        report = hits({"lib/a.cpp": {40: 0, 41: 0}})
        changed = {"lib/a.cpp": {40: "    a();", 41: "    brand_new();"}}
        carried = {"lib/a.cpp": {cg.normalise("a();")}}
        result = cg.patch_coverage(report, changed, carried)
        self.assertEqual(result.carried, 1)
        self.assertEqual(result.total, 1)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 41)])

    def test_genuinely_new_uncovered_lines_still_fail(self):
        report = hits({"lib/a.cpp": {40: 0}})
        changed = {"lib/a.cpp": {40: "    brand_new_uncovered_call();"}}
        result = cg.patch_coverage(report, changed, {"lib/a.cpp": set()})
        self.assertEqual(result.total, 1)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 40)])
        self.assertFalse(result.passes)

    def test_carryover_is_reported_rather_than_silently_dropped(self):
        report = hits({"lib/a.cpp": {40: 0}})
        changed = {"lib/a.cpp": {40: "    moved();"}}
        buf = io.StringIO()
        with redirect_stdout(buf):
            cg.check(cg.patch_coverage(report, changed, {"lib/a.cpp": {"moved();"}}), {})
        self.assertIn("already existed elsewhere in the file", buf.getvalue())

    def test_reindented_move_still_counts_as_moved(self):
        report = hits({"lib/a.cpp": {40: 0}})
        changed = {"lib/a.cpp": {40: "            deep_indent();"}}
        carried = {"lib/a.cpp": {cg.normalise("deep_indent();")}}
        self.assertEqual(cg.patch_coverage(report, changed, carried).carried, 1)

    def test_normalise_collapses_whitespace_only(self):
        self.assertEqual(cg.normalise("  a  \t b "), cg.normalise("a b"))
        self.assertNotEqual(cg.normalise("a;"), cg.normalise("b;"))


class Threshold(unittest.TestCase):
    def test_threshold_is_eighty(self):
        self.assertEqual(cg.COVERAGE_MIN, 80)

    def test_gate_fails_below_threshold(self):
        report = hits({"lib/a.cpp": [(n, 0 if n < 8 else 1) for n in range(1, 11)]})
        changed = chg(("lib/a.cpp", list(range(1, 11))))
        rc, out = run_check(cg.patch_coverage(report, changed), changed)
        self.assertEqual(rc, 1)
        self.assertIn("80", out)

    def test_gate_passes_at_threshold(self):
        report = hits({"lib/a.cpp": [(n, 1 if n < 9 else 0) for n in range(1, 11)]})
        changed = chg(("lib/a.cpp", list(range(1, 11))))
        rc, _ = run_check(cg.patch_coverage(report, changed), changed)
        self.assertEqual(rc, 0, "exactly 80% must pass; the bar is >= not >")

    def test_gate_passes_when_nothing_changed(self):
        rc, out = run_check(cg.patch_coverage(hits({"lib/a.cpp": {10: 0}}), {}), {})
        self.assertEqual(rc, 0, out)
        self.assertIn("no changed", out.lower())


class FailClosed(unittest.TestCase):
    def test_missing_report_file_fails_closed(self):
        rc, out = run_check(cg.patch_coverage(None, chg(("lib/a.cpp", [10]))), chg(("lib/a.cpp", [10])))
        self.assertEqual(rc, 2)
        self.assertIn("report", out.lower())

    def test_empty_report_fails_closed(self):
        rc, out = run_check(cg.patch_coverage({"files": []}, chg(("lib/a.cpp", [10]))),
                           chg(("lib/a.cpp", [10])))
        self.assertEqual(rc, 2)

    def test_no_diff_fails_closed(self):
        """No changed-line set means we cannot tell covered from uncovered, and
        reporting 100% would be a lie."""
        rc, out = run_check(cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), None), None)
        self.assertEqual(rc, 2)
        self.assertIn("diff", out.lower())

    def test_empty_mapping_fails_closed(self):
        rc, _ = run_check(cg.patch_coverage(hits({}), chg(("lib/a.cpp", [10]))),
                          chg(("lib/a.cpp", [10])))
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
            result = cg.patch_coverage(cg.read_report(path), chg(("lib/a.cpp", [10, 11])))
        finally:
            os.unlink(path)
        self.assertEqual(result.total, 2)
        self.assertEqual(result.covered, 1)
        self.assertFalse(result.passes)


class ChangedLines(unittest.TestCase):
    def test_carries_the_added_line_text(self):
        diff = ["+++ b/lib/a.cpp", "@@ -10,1 +10,2 @@", " ctx", "+    int x = 1;"]
        self.assertEqual(cg.changed_lines_from_diff(diff)["lib/a.cpp"], {11: "    int x = 1;"})

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
        self.assertEqual(sorted(changed["lib/a.cpp"]), [11, 12])

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
        self.assertEqual(cg.changed_lines_from_diff(diff)["lib/a.cpp"],
                         {1: "one", 21: "two"})

    def test_ignores_files_with_no_added_lines(self):
        diff = ["+++ b/lib/a.cpp", "@@ -1,2 +1,2 @@", " ctx", "-gone"]
        self.assertNotIn("lib/a.cpp", cg.changed_lines_from_diff(diff))

    def test_ignores_dev_null_targets(self):
        self.assertEqual(cg.changed_lines_from_diff(["+++ /dev/null"]), {})

    def test_new_file_counts_all_its_lines(self):
        diff = ["--- /dev/null", "+++ b/lib/new.cpp", "@@ -0,0 +1,3 @@", "+a", "+b", "+c"]
        self.assertEqual(sorted(cg.changed_lines_from_diff(diff)["lib/new.cpp"]), [1, 2, 3])

    def test_binary_files_are_skipped(self):
        diff = ["+++ b/logo.png", "Binary files /dev/null and b/logo.png differ"]
        self.assertEqual(cg.changed_lines_from_diff(diff), {})


if __name__ == "__main__":
    unittest.main()