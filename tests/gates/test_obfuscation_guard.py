"""The obfuscation guard is a gating CI job and had no tests.

Two rules carry the gate: a known obfuscator marker is a finding in any scannable
file, and a code line past `MAX_LINE` is a finding. The length rule must stay
scoped to code (tracked data legitimately has long lines), and the guard must not
flag its own source while naming the markers it looks for.
"""

import os
import tempfile
import unittest

from harness import REPO_ROOT, load

guard = load("obfuscation_guard")
TOOL = os.path.join(REPO_ROOT, "tools", "obfuscation_guard.py")


class Scope(unittest.TestCase):
    def test_code_and_data_files_are_scanned(self):
        for path in ("lib/a.cpp", "plugins/p/x.js", "website/x.astro",
                     "docs/index.html", "completions.json"):
            self.assertTrue(guard.scannable(path), path)

    def test_vendored_generated_and_unknown_files_are_skipped(self):
        for path in ("third_party/md4c/md4c.c", "website/node_modules/x.js",
                     "vendor/x.h", "include/nlohmann/json.hpp",
                     "package-lock.json", "README.md", "amber"):
            self.assertFalse(guard.scannable(path), path)


class Findings(unittest.TestCase):
    def scan(self, name, text):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, name), "w", encoding="utf-8") as handle:
                handle.write(text)
            return guard.scan_file(root, name)

    def test_a_known_marker_is_a_finding(self):
        found = self.scan("a.cpp", "var x = " + guard.MARKERS[0] + ";\n")
        self.assertTrue(found)
        self.assertIn("obfuscator marker", found[0][1])

    def test_the_length_limit_is_inclusive(self):
        self.assertEqual(self.scan("a.cpp", "x" * guard.MAX_LINE + "\n"), [])

    def test_one_character_over_the_limit_is_a_finding(self):
        found = self.scan("a.cpp", "x" * (guard.MAX_LINE + 1) + "\n")
        self.assertEqual(len(found), 1)
        self.assertIn("char line", found[0][1])

    def test_the_length_rule_does_not_apply_to_tracked_data(self):
        self.assertEqual(self.scan("data.json", "x" * (guard.MAX_LINE + 1) + "\n"), [])

    def test_an_unreadable_file_is_reported_not_skipped(self):
        with tempfile.TemporaryDirectory() as root:
            found = guard.scan_file(root, "missing.cpp")
        self.assertEqual(len(found), 1)
        self.assertIn("unreadable", found[0][1])

    def test_a_clean_file_has_no_findings(self):
        self.assertEqual(self.scan("a.cpp", "int main() { return 0; }\n"), [])


class SelfScan(unittest.TestCase):
    def test_the_guard_does_not_contain_a_marker_as_one_literal(self):
        """The markers are assembled with `_m()` for this reason: a contiguous
        marker in this file would make the guard flag its own source."""
        with open(TOOL, encoding="utf-8") as handle:
            source = handle.read()
        for marker in guard.MARKERS:
            self.assertNotIn(marker, source)


if __name__ == "__main__":
    unittest.main()
