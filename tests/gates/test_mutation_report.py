"""The mutation-report summariser.

The nightly mutation job is advisory, but its report still has to be honest: a
timeout is not a kill, an empty report is not a clean run, and an unknown
execution status means the schema changed underneath the reader.
"""

import io
import os
import sqlite3
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

mutation = load("mutation_report")


def write_report(rows):
    handle, path = tempfile.mkstemp(suffix=".sqlite")
    os.close(handle)
    connection = sqlite3.connect(path)
    connection.execute(
        "create table mutant (mutant_id integer primary key, mutator text, "
        "filename text, line_number integer, execution_status integer, "
        "mutation_replacement text)")
    connection.executemany(
        "insert into mutant (mutator, filename, line_number, execution_status, "
        "mutation_replacement) values (?, ?, ?, ?, ?)", rows)
    connection.commit()
    connection.close()
    return path


def run_main(*argv):
    out, err = io.StringIO(), io.StringIO()
    old = __import__("sys").argv
    __import__("sys").argv = ["mutation_report.py", *argv]
    try:
        with redirect_stdout(out), redirect_stderr(err):
            rc = mutation.main()
    finally:
        __import__("sys").argv = old
    return rc, out.getvalue() + err.getvalue()


class Summary(unittest.TestCase):
    ROWS = [
        ("cxx_gt_to_ge", "lib/a.cpp", 10, mutation.KILLED, ">="),
        ("cxx_lt_to_le", "lib/a.cpp", 12, mutation.KILLED, "<="),
        ("cxx_add_to_sub", "lib/b.cpp", 3, mutation.KILLED, "-"),
        ("cxx_eq_to_ne", "plugins/c.cpp", 7, mutation.SURVIVED, "!="),
        ("cxx_mul_to_div", "plugins/c.cpp", 9, mutation.TIMEOUT, "/"),
    ]

    def setUp(self):
        self.path = write_report(self.ROWS)

    def tearDown(self):
        os.unlink(self.path)

    def test_counts_keep_timeouts_separate_from_kills(self):
        result, error = mutation.summarise(self.ROWS)
        self.assertIsNone(error)
        self.assertEqual(result["total"], 5)
        self.assertEqual(result["counts"],
                         {"killed": 3, "survived": 1, "timeout": 1})
        self.assertAlmostEqual(result["conservative_score"], 60.0)
        self.assertAlmostEqual(result["mull_score"], 80.0)

    def test_the_survivor_list_names_the_file_and_mutant(self):
        result, _ = mutation.summarise(self.ROWS)
        self.assertEqual(result["survivors"], [
            {"file": "plugins/c.cpp", "line": 7, "mutator": "cxx_eq_to_ne",
             "replacement": "!="},
        ])
        text = mutation.markdown(result, "origin/main")
        self.assertIn("`plugins/c.cpp:7`", text)
        self.assertIn("cxx_eq_to_ne", text)
        self.assertIn("60.0%", text)

    def test_an_unknown_status_fails_closed(self):
        result, error = mutation.summarise([("m", "f", 1, 99, "x")])
        self.assertIsNone(result)
        self.assertIn("schema changed", error)

    def test_an_empty_report_fails_when_mutants_were_required(self):
        result, error = mutation.summarise([], require_mutants=True)
        self.assertIsNone(result)
        self.assertIn("no mutants", error)
        result, error = mutation.summarise([], require_mutants=False)
        self.assertIsNone(error)
        self.assertEqual(result["total"], 0)

    def test_a_clean_report_says_so(self):
        text = mutation.markdown(mutation.summarise(self.ROWS[:3])[0], "HEAD")
        self.assertIn("No surviving mutants.", text)


class CommandLine(unittest.TestCase):
    def test_a_missing_report_fails_closed(self):
        rc, out = run_main("--report", "/nonexistent/mutation.sqlite")
        self.assertEqual(rc, 2)
        self.assertIn("FAILED CLOSED", out)

    def test_require_mutants_is_an_error_on_an_empty_report(self):
        path = write_report([])
        try:
            rc, out = run_main("--report", path, "--require-mutants")
        finally:
            os.unlink(path)
        self.assertEqual(rc, 2)
        self.assertIn("no mutants", out)

    def test_the_markdown_file_is_written(self):
        path = write_report(Summary.ROWS)
        handle, markdown = tempfile.mkstemp(suffix=".md")
        os.close(handle)
        try:
            rc, _ = run_main("--report", path, "--markdown", markdown, "--base", "abc123")
            with open(markdown, encoding="utf-8") as report:
                text = report.read()
        finally:
            os.unlink(path)
            os.unlink(markdown)
        self.assertEqual(rc, 0)
        self.assertIn("Base: `abc123`", text)

    def test_an_optional_score_floor_can_fail_the_run(self):
        path = write_report(Summary.ROWS)
        try:
            self.assertEqual(run_main("--report", path, "--min-score", "50")[0], 0)
            rc, out = run_main("--report", path, "--min-score", "90")
        finally:
            os.unlink(path)
        self.assertEqual(rc, 1)
        self.assertIn("below the 90.0% floor", out)


if __name__ == "__main__":
    unittest.main()
