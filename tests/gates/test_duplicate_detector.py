"""The duplicate detector is a gating CI job and had no tests.

A detector that stops normalising (or that reports same-file repetition) turns
either into a noisy gate that gets waived or a silent one that passes. These
cases pin the two rules the gate depends on: normalisation ignores comments,
strings and numbers, and a report requires the same block in *different* files.
"""

import os
import subprocess
import sys
import tempfile
import unittest

from harness import REPO_ROOT, load

duplicates = load("duplicate_detector")
TOOL = os.path.join(REPO_ROOT, "tools", "duplicate_detector.py")

BLOCK = """\
void helper(int value) {
    int first = value + 1;
    int second = value + 2;
    // the same comment in both files
    use(first, second);
}
"""


class Normalisation(unittest.TestCase):
    def test_line_comments_are_ignored(self):
        self.assertEqual(duplicates.normalize("int x = 1; // note"), "int x = 0;")

    def test_string_literals_are_ignored(self):
        self.assertEqual(duplicates.normalize('const char* s = "hello";'),
                         'const char* s = "";')

    def test_numbers_are_ignored(self):
        self.assertEqual(duplicates.normalize("return 42;"), "return 0;")

    def test_surrounding_whitespace_is_ignored(self):
        self.assertEqual(duplicates.normalize("    call();   "), "call();")


class Blocks(unittest.TestCase):
    def test_a_block_at_or_above_the_minimum_is_measured(self):
        blocks = duplicates.extract_blocks(BLOCK, min_lines=6)
        self.assertEqual(len(blocks), 1)

    def test_a_block_below_the_minimum_is_not_measured(self):
        self.assertEqual(duplicates.extract_blocks(BLOCK, min_lines=7), [])

    def test_normalisation_makes_comments_and_numbers_irrelevant(self):
        other = BLOCK.replace("value + 1", "value + 99").replace(
            "the same comment", "a different comment")
        first = duplicates.extract_blocks(BLOCK, min_lines=6)[0][0]
        second = duplicates.extract_blocks(other, min_lines=6)[0][0]
        self.assertEqual(first, second)

    def test_a_real_change_changes_the_signature(self):
        other = BLOCK.replace("use(first, second);", "consume(first, second);")
        first = duplicates.extract_blocks(BLOCK, min_lines=6)[0][0]
        second = duplicates.extract_blocks(other, min_lines=6)[0][0]
        self.assertNotEqual(first, second)


class TheGate(unittest.TestCase):
    def run_tool(self, directory, *extra):
        return subprocess.run(
            [sys.executable, TOOL, "--min-lines=6", *extra, directory],
            capture_output=True, text=True,
        )

    def write(self, directory, name, text):
        with open(os.path.join(directory, name), "w", encoding="utf-8") as handle:
            handle.write(text)

    def test_the_same_block_in_two_files_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write(tmp, "a.cpp", BLOCK)
            self.write(tmp, "b.cpp", BLOCK)
            proc = self.run_tool(tmp)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn("a.cpp", proc.stdout)
            self.assertIn("b.cpp", proc.stdout)

    def test_unique_files_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write(tmp, "a.cpp", BLOCK)
            self.write(tmp, "b.cpp", BLOCK.replace("helper", "other").replace(
                "use(first, second);", "consume(first, second);"))
            self.assertEqual(self.run_tool(tmp).returncode, 0)

    def test_an_excluded_path_cannot_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write(tmp, "a.cpp", BLOCK)
            self.write(tmp, "b.cpp", BLOCK)
            proc = self.run_tool(tmp, "--exclude=b\\.cpp")
            self.assertEqual(proc.returncode, 0, proc.stdout)


if __name__ == "__main__":
    unittest.main()
