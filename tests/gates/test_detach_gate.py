"""The detach ratchet: existing sites are recorded, a new one fails.

A detached thread outlives its owner, so its captures can be freed while it runs
and there is nothing to join at shutdown. The tree already has five call sites,
so the gate has to be a ratchet; these tests pin that it measures code rather
than comments, and that it fails on growth and on a new file.
"""

import os
import unittest

from harness import REPO_ROOT, load

detach = load("detach_gate")


class Counting(unittest.TestCase):
    def test_a_detach_call_is_counted(self):
        self.assertEqual(detach.count_detaches("t.detach();\n"), 1)

    def test_two_calls_are_two(self):
        self.assertEqual(detach.count_detaches("a.detach();\nb.detach();\n"), 2)

    def test_a_comment_is_not_a_call(self):
        self.assertEqual(detach.count_detaches("// t.detach();\n"), 0)

    def test_a_string_literal_is_not_a_call(self):
        self.assertEqual(detach.count_detaches('log("t.detach()");\n'), 0)

    def test_a_similar_name_is_not_a_call(self):
        self.assertEqual(detach.count_detaches("detachment();\n"), 0)


class Ratchet(unittest.TestCase):
    def test_a_new_file_with_a_detach_fails(self):
        bad = detach.regressions({"new.cpp": 1}, {})
        self.assertEqual(bad, [("new.cpp", 1, 0)])

    def test_growth_in_a_recorded_file_fails(self):
        bad = detach.regressions({"a.cpp": 2}, {"a.cpp": 1})
        self.assertEqual(bad, [("a.cpp", 2, 1)])

    def test_the_recorded_count_is_allowed(self):
        self.assertEqual(detach.regressions({"a.cpp": 1}, {"a.cpp": 1}), [])

    def test_a_reduction_is_allowed(self):
        self.assertEqual(detach.regressions({"a.cpp": 1}, {"a.cpp": 2}), [])

    def test_a_file_that_dropped_to_zero_is_not_a_regression(self):
        self.assertEqual(detach.regressions({}, {"a.cpp": 1}), [])

    def test_a_scan_that_read_almost_nothing_fails_closed(self):
        self.assertEqual(detach.check({}, {}, scanned=0), 2)


class TheTree(unittest.TestCase):
    def test_the_tree_has_no_site_above_the_baseline(self):
        scanned = detach.scanned_files()
        current = detach.current_counts()
        self.assertGreaterEqual(scanned, detach.MIN_FILES_SANE)
        self.assertEqual(detach.regressions(current, detach.load_baseline()), [])

    def test_the_baseline_is_tracked(self):
        self.assertTrue(os.path.exists(os.path.join(REPO_ROOT, "tests", "detach_baseline.json")))


if __name__ == "__main__":
    unittest.main()
