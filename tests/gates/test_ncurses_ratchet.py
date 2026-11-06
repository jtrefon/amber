"""The ncurses ratchet: the exception list only shrinks.

The gate is only believable if a new include fails, a listed one passes, a
removal can be recorded, and a scan that read almost nothing fails closed.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from harness import load

ratchet = load("ncurses_ratchet")


def run_check(found, baseline, scanned, min_files=1):
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = ratchet.check(found, set(baseline), scanned, min_files=min_files)
    return code, buf.getvalue()


def write(root, name, text):
    path = os.path.join(root, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


class Detection(unittest.TestCase):
    def test_include_spellings_are_all_detected(self):
        for spelling in ("ncurses.h", "ncursesw.h", "panel.h", "form.h", "menu.h"):
            self.assertEqual(len(ratchet.INCLUDE.findall(f"#include <{spelling}>\n")), 1)

    def test_a_comment_or_a_string_is_not_an_include(self):
        for text in ("// #include <ncurses.h>\n", 'const char* s = "#include <ncurses.h>";\n',
                     "#include <curses.h>\n"):
            self.assertEqual(ratchet.INCLUDE.findall(text), [])

    def test_scan_reports_line_numbers(self):
        with tempfile.TemporaryDirectory() as root:
            write(root, "a.cpp", "int x;\n#include <ncurses.h>\n")
            write(root, "b.cpp", "int y;\n")
            found, scanned = ratchet.scan(root)
        self.assertEqual(found, {"a.cpp": [2]})
        self.assertEqual(scanned, 2)


class Ratchet(unittest.TestCase):
    def test_a_baselined_file_passes(self):
        code, out = run_check({"tui/widgets.h": [3]}, ["tui/widgets.h"], 10)
        self.assertEqual(code, 0, out)
        self.assertIn("clean", out)

    def test_a_new_include_fails_and_names_the_file(self):
        code, out = run_check(
            {"tui/widgets.h": [3], "tui/new_thing.cpp": [7]}, ["tui/widgets.h"], 10
        )
        self.assertEqual(code, 1, out)
        self.assertIn("tui/new_thing.cpp", out)
        self.assertIn("line 7", out)

    def test_a_removal_passes_but_is_reported_as_shrinkable(self):
        code, out = run_check({"tui/one.h": [1]}, ["tui/one.h", "tui/two.h"], 10)
        self.assertEqual(code, 0, out)
        self.assertIn("no longer includes ncurses", out)

    def test_a_scan_that_read_almost_nothing_fails_closed(self):
        code, out = run_check({}, [], 1, min_files=200)
        self.assertEqual(code, 2, out)
        self.assertIn("FAILED CLOSED", out)


class Baseline(unittest.TestCase):
    def test_update_writes_a_sorted_list_that_check_accepts(self):
        with tempfile.TemporaryDirectory() as root:
            write(root, "b.h", "#include <ncurses.h>\n")
            write(root, "a.cpp", "#include <panel.h>\n")
            found, scanned = ratchet.scan(root)
            path = os.path.join(root, "baseline.json")
            with redirect_stdout(io.StringIO()):
                ratchet.update(path, found)
            with open(path, encoding="utf-8") as handle:
                payload = json.load(handle)
            self.assertEqual(payload["files"], ["a.cpp", "b.h"])
            self.assertEqual(ratchet.load_baseline(path), {"a.cpp", "b.h"})
            code, out = run_check(found, payload["files"], scanned)
            self.assertEqual(code, 0, out)

    def test_a_missing_baseline_is_an_error(self):
        with self.assertRaises(RuntimeError):
            ratchet.load_baseline("/nonexistent/baseline.json")

    def test_a_baseline_without_a_files_list_is_an_error(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "bad.json")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write('{"nope": []}')
            with self.assertRaises(RuntimeError):
                ratchet.load_baseline(path)


if __name__ == "__main__":
    unittest.main()
