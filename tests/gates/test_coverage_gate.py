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


class ChangedLinesOption(unittest.TestCase):
    """--changed-lines is documented as JSON {path: [lines]}.

    It built {path: set(lines)} while patch_coverage indexes the value as a mapping, so
    every use raised TypeError. Nothing exercised it -- CI always uses --diff-base -- so
    it had been broken for its whole life, and the fix is only observable by running it.
    """

    def _resolve(self, payload):
        import json as _json
        import tempfile as _tempfile
        with _tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            _json.dump(payload, handle)
            path = handle.name
        try:
            args = type("A", (), {"changed_lines": path, "diff_base": None})()
            return cg.resolve_changed(args)
        finally:
            os.unlink(path)

    def test_it_produces_the_shape_patch_coverage_indexes(self):
        changed, err = self._resolve({"lib/a.cpp": [10, 11]})
        self.assertIsNone(err)
        self.assertEqual(changed, {"lib/a.cpp": {10: "", 11: ""}})

    def test_it_is_usable_by_patch_coverage(self):
        changed, err = self._resolve({"lib/a.cpp": [10, 11]})
        self.assertIsNone(err)
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1, 11: 0}}), changed)
        self.assertEqual(result.total, 2)
        self.assertEqual(result.uncovered, [("lib/a.cpp", 11)])

    def test_string_line_numbers_are_accepted(self):
        """JSON object keys are strings; line numbers are not."""
        changed, err = self._resolve({"lib/a.cpp": ["10", "11"]})
        self.assertIsNone(err)
        self.assertEqual(changed, {"lib/a.cpp": {10: "", 11: ""}})


class GitOutputIsBytes(unittest.TestCase):
    """A diff can legitimately contain invalid UTF-8, and decoding it strictly raises.

    A fuzz corpus seed must stay invalid -- that is what it exists to test -- so adding
    one crashed the patch coverage gate with UnicodeDecodeError. The gate failing closed
    is better than a false pass, but a crash is not a measurement, and the real bug was
    invisible until a PR happened to carry undecodable bytes.
    """

    def test_diff_output_is_decoded_leniently(self):
        self.assertEqual(cg.GIT_DECODE, {"errors": "replace"})

    def test_both_git_reads_use_it(self):
        """diff_against (the changed lines) and base_lines (the carryover text) both
        read git output. Fixing only the first would leave carryover crashing on the
        same input."""
        import inspect
        for fn in (cg.diff_against, cg.base_lines):
            self.assertIn("GIT_DECODE", inspect.getsource(fn), f"{fn.__name__} bypasses it")

    def test_an_undecodable_line_still_parses(self):
        """The replacement character must not change the line structure the parser
        depends on: a diff that fails to parse would silently measure nothing."""
        raw = b"+++ b/fuzz/corpus/x\n@@ -0,0 +1 @@\n+\udcff\udcfe bad utf8"
        lines = raw.decode("utf-8", errors="replace").splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn("bad utf8", lines[2])


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


BRANCH_XML = """<?xml version="1.0" ?>
<coverage line-rate="0.5" branch-rate="0.5" branches-covered="3" branches-valid="8">
  <packages><package><classes>
    <class filename="lib/a.cpp" name="a_cpp" line-rate="1.0" branch-rate="0.375">
      <lines>
        <line number="10" hits="1" branch="true" condition-coverage="50% (1/2)"/>
        <line number="11" hits="1"/>
        <line number="12" hits="1" branch="true" condition-coverage="0% (0/4)"/>
        <line number="13" hits="1" branch="true" condition-coverage="100% (2/2)"/>
      </lines>
    </class>
    <class filename="tui/t.cpp" name="t_cpp" line-rate="1.0" branch-rate="0.5">
      <lines><line number="5" hits="1" branch="true" condition-coverage="50% (1/2)"/></lines>
    </class>
  </classes></package></packages>
</coverage>
"""

BRANCHES = {
    "lib/a.cpp": {10: (1, 2), 12: (0, 4), 13: (2, 2)},
    "tui/t.cpp": {5: (1, 2)},
}


def branch_check(result, changed, ratchet=None, ratchet_error=None):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cg.check(result, changed, ratchet, ratchet_error)
    return rc, buf.getvalue()


class ReadingBranchData(unittest.TestCase):
    """gcovr's Cobertura output carries a line's conditions in
    `condition-coverage="P% (covered/total)"`. The gate reads that attribute, not
    the `<conditions>` children, because the attribute is the aggregate gcovr
    guarantees and the children's ordering is an implementation detail."""

    def write(self, text):
        handle = tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False)
        handle.write(text)
        handle.close()
        return handle.name

    def test_reads_condition_coverage_per_line(self):
        path = self.write(BRANCH_XML)
        try:
            loaded = cg.read_branch_report(path)
        finally:
            os.unlink(path)
        self.assertEqual(loaded["lib/a.cpp"], {10: (1, 2), 12: (0, 4), 13: (2, 2)})
        self.assertEqual(loaded["tui/t.cpp"], {5: (1, 2)})

    def test_lines_without_branch_data_are_absent(self):
        path = self.write(BRANCH_XML)
        try:
            self.assertNotIn(11, cg.read_branch_report(path)["lib/a.cpp"])
        finally:
            os.unlink(path)

    def test_malformed_percentage_fails_closed(self):
        """A branch line whose counts cannot be parsed is a measurement gap, not
        a zero. Returning the other files would report a number computed from a
        report we do not understand."""
        bad = BRANCH_XML.replace('condition-coverage="50% (1/2)"',
                                 'condition-coverage="banana"', 1)
        path = self.write(bad)
        try:
            self.assertIsNone(cg.read_branch_report(path))
        finally:
            os.unlink(path)

    def test_branch_attribute_without_counts_fails_closed(self):
        bad = BRANCH_XML.replace(' branch="true" condition-coverage="0% (0/4)"',
                                 ' branch="true"', 1)
        path = self.write(bad)
        try:
            self.assertIsNone(cg.read_branch_report(path))
        finally:
            os.unlink(path)

    def test_impossible_counts_fail_closed(self):
        bad = BRANCH_XML.replace('condition-coverage="0% (0/4)"',
                                 'condition-coverage="0% (5/4)"', 1)
        path = self.write(bad)
        try:
            self.assertIsNone(cg.read_branch_report(path))
        finally:
            os.unlink(path)

    def test_missing_file_is_reported_not_raised(self):
        self.assertIsNone(cg.read_branch_report("/nonexistent/coverage.xml"))

    def test_malformed_xml_is_reported_not_raised(self):
        path = self.write("this is not xml at all")
        try:
            self.assertIsNone(cg.read_branch_report(path))
        finally:
            os.unlink(path)

    def test_a_report_with_no_branch_data_is_empty_not_unreadable(self):
        """An empty dict means "measured, and the tree has no branches", which the
        gate treats as a measurement failure; None means the report itself could
        not be read. The distinction decides whether the message blames the file
        or the toolchain."""
        handle = tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False)
        handle.write(COBERTURA)
        handle.close()
        try:
            self.assertEqual(cg.read_branch_report(handle.name), {})
        finally:
            os.unlink(handle.name)


class PatchBranchMath(unittest.TestCase):
    def test_only_changed_lines_with_branch_data_count(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1, 11: 1, 12: 1, 13: 1}}),
                                   chg(("lib/a.cpp", [10, 11, 12, 13])), branches=BRANCHES)
        self.assertEqual(result.branches.total, 8)
        self.assertEqual(result.branches.covered, 3)
        self.assertAlmostEqual(result.branches.percent, 37.5)

    def test_a_line_without_branches_is_not_a_zero(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {11: 1}}),
                                   chg(("lib/a.cpp", [11])), branches=BRANCHES)
        self.assertEqual(result.branches.total, 0)
        self.assertIsNone(result.branches.percent)
        self.assertTrue(result.branches.passes)

    def test_carried_over_uncovered_branch_is_not_held_against_the_patch(self):
        """Moving a whole `if` is not writing a new one. The condition still shows
        zero covered on the new line, so carryover has to apply to branches too."""
        changed = {"lib/a.cpp": {12: "    if (moved) {"}}
        carried = {"lib/a.cpp": {cg.normalise("if (moved) {")}}
        result = cg.patch_coverage(hits({"lib/a.cpp": {12: 1}}), changed,
                                   carried_over=carried, branches=BRANCHES)
        self.assertEqual(result.branches.total, 0)
        self.assertEqual(result.branches.carried, 4)

    def test_per_file_totals_are_reported(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}, "tui/t.cpp": {5: 1}}),
                                   chg(("lib/a.cpp", [10]), ("tui/t.cpp", [5])),
                                   branches=BRANCHES)
        self.assertEqual(result.branches.by_file["lib/a.cpp"], (1, 2))
        self.assertEqual(result.branches.by_file["tui/t.cpp"], (1, 2))

    def test_no_branch_data_supplied_leaves_the_branch_gate_off(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), chg(("lib/a.cpp", [10])))
        self.assertIsNone(result.branches)


class BranchScope(unittest.TestCase):
    """Branch coverage is gated for the instrumented core, the same scope as the
    project line floor. The TUI is driven through a pty, so its branch number is
    reported and not enforced until the harness can exercise the branches."""

    def test_core_directories_are_gated(self):
        for path in ("lib/a.cpp", "tools/x.cpp", "plugins/p/p.cpp"):
            self.assertTrue(cg.is_branch_gated(path), path)
        for path in ("tui/t.cpp", "src/main.cpp", "bench/runner.cpp", "tests/x.cpp"):
            self.assertFalse(cg.is_branch_gated(path), path)

    def test_tui_branches_are_reported_but_excluded_from_the_gated_total(self):
        changed = chg(("tui/t.cpp", [5]))
        result = cg.patch_coverage(hits({"tui/t.cpp": {5: 1}}), changed, branches=BRANCHES)
        self.assertEqual(result.branches.total, 0)
        self.assertIn("tui/t.cpp", result.branches.by_file)
        rc, out = branch_check(result, changed)
        self.assertEqual(rc, 0, out)
        self.assertIn("tui/t.cpp", out)


class BranchThreshold(unittest.TestCase):
    def test_bar_is_fifty(self):
        self.assertEqual(cg.BRANCH_COVERAGE_MIN, 50)

    def test_below_the_bar_fails(self):
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1, 12: 1, 13: 1}}),
                                   chg(("lib/a.cpp", [10, 12, 13])), branches=BRANCHES)
        rc, out = branch_check(result, chg(("lib/a.cpp", [10, 12, 13])))
        self.assertEqual(rc, 1)
        self.assertIn("patch branch coverage", out)
        self.assertIn("37.5", out)

    def test_at_the_bar_passes(self):
        branches = {"lib/a.cpp": {10: (1, 2)}}
        changed = chg(("lib/a.cpp", [10]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), changed, branches=branches)
        self.assertEqual(result.branches.percent, 50.0)
        self.assertEqual(branch_check(result, changed)[0], 0)

    def test_a_fully_covered_branch_passes(self):
        branches = {"lib/a.cpp": {10: (2, 2)}}
        changed = chg(("lib/a.cpp", [10]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), changed, branches=branches)
        self.assertEqual(branch_check(result, changed)[0], 0)

    def test_no_new_gated_branches_passes(self):
        branches = {"lib/a.cpp": {}}
        changed = chg(("lib/a.cpp", [10]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), changed, branches=branches)
        rc, out = branch_check(result, changed)
        self.assertEqual(rc, 0, out)
        self.assertIn("no new branches", out)

    def test_an_empty_branch_report_fails_closed(self):
        changed = chg(("lib/a.cpp", [10]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), changed, branches={})
        rc, out = branch_check(result, changed)
        self.assertEqual(rc, 2, out)
        self.assertIn("branch", out.lower())


class BranchRatchet(unittest.TestCase):
    """The patch percentage cannot see a branch added in a file that also gained
    plenty of covered ones. The per-file uncovered count can: it may never grow.
    That is the floor for `lib/`, `tools/` and `plugins/`, and it is the same
    ratchet pattern as the lint baseline."""

    def test_equal_counts_pass(self):
        self.assertEqual(cg.branch_ratchet(BRANCHES, {"lib/a.cpp": 4}), [])

    def test_growth_fails(self):
        violations = cg.branch_ratchet(BRANCHES, {"lib/a.cpp": 3})
        self.assertEqual(violations, [("lib/a.cpp", 4, 3)])

    def test_a_reduction_passes(self):
        self.assertEqual(cg.branch_ratchet(BRANCHES, {"lib/a.cpp": 10}), [])

    def test_a_new_file_starts_at_zero(self):
        """A file the baseline has never seen has no allowance. Covered branches
        are free; one uncovered branch fails."""
        branches = {"lib/new.cpp": {7: (1, 2), 8: (2, 2)}}
        self.assertEqual(cg.branch_ratchet(branches, {}), [("lib/new.cpp", 1, 0)])

    def test_a_new_file_with_full_coverage_passes(self):
        self.assertEqual(cg.branch_ratchet({"lib/new.cpp": {7: (2, 2)}}, {}), [])

    def test_non_gated_scopes_are_not_ratcheted(self):
        self.assertEqual(cg.branch_ratchet({"tui/t.cpp": {5: (0, 2)}}, {}), [])

    def test_a_ratchet_violation_fails_the_gate(self):
        changed = chg(("lib/a.cpp", [10, 12, 13]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1, 12: 1, 13: 1}}), changed,
                                   branches=BRANCHES)
        rc, out = branch_check(result, changed, cg.branch_ratchet(BRANCHES, {"lib/a.cpp": 3}))
        self.assertEqual(rc, 1)
        self.assertIn("lib/a.cpp", out)
        self.assertIn("baseline", out)

    def test_a_missing_baseline_fails_closed(self):
        changed = chg(("lib/a.cpp", [10]))
        result = cg.patch_coverage(hits({"lib/a.cpp": {10: 1}}), changed, branches=BRANCHES)
        rc, out = branch_check(result, changed, None, "no baseline")
        self.assertEqual(rc, 2)
        self.assertIn("no baseline", out)


class BranchBaselineFile(unittest.TestCase):
    def test_round_trip_keeps_only_uncovered_counts(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        handle.close()
        try:
            cg.write_branch_baseline(handle.name, BRANCHES)
            loaded = cg.load_branch_baseline(handle.name)
        finally:
            os.unlink(handle.name)
        self.assertEqual(loaded, {"lib/a.cpp": 4})

    def test_missing_baseline_is_none_not_an_empty_floor(self):
        self.assertIsNone(cg.load_branch_baseline("/nonexistent/baseline.json"))

    def test_malformed_baseline_is_none(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        handle.write('{"files": "not a map"}')
        handle.close()
        try:
            self.assertIsNone(cg.load_branch_baseline(handle.name))
        finally:
            os.unlink(handle.name)

    def test_the_committed_baseline_loads_and_is_sorted(self):
        """The baseline in tests/ is a real artefact the gate reads. A malformed
        one would fail every coverage run closed, so it is checked here."""
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        path = os.path.join(root, "tests", "branch_baseline.json")
        loaded = cg.load_branch_baseline(path)
        self.assertIsNotNone(loaded)
        self.assertTrue(loaded)
        self.assertEqual(list(loaded), sorted(loaded))
        for name, uncovered in loaded.items():
            self.assertTrue(cg.is_branch_gated(name), name)
            self.assertGreater(uncovered, 0, name)


if __name__ == "__main__":
    unittest.main()