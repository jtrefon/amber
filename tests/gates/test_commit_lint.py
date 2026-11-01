"""Commit hygiene is advisory, so the tests pin the rules and the exemptions
rather than an exit code that blocks a merge.
"""

import unittest

from harness import load

commit = load("commit_lint")


class Subjects(unittest.TestCase):
    def test_a_scoped_subject_passes(self):
        self.assertEqual(commit.lint_subjects(["tui: fix drawer scroll"]), [])

    def test_an_unscoped_subject_is_reported(self):
        problems = commit.lint_subjects(["fixed the drawer"])
        self.assertEqual(len(problems), 1)
        self.assertIn("area", problems[0][2])

    def test_a_conventional_commit_is_reported_as_a_different_convention(self):
        problems = commit.lint_subjects(["fix(coding): make the library answerable"])
        self.assertEqual(len(problems), 1)

    def test_a_long_subject_is_reported(self):
        problems = commit.lint_subjects(["tui: " + "x" * commit.MAX_SUBJECT])
        self.assertEqual(len(problems), 1)
        self.assertIn("characters", problems[0][2])

    def test_merge_revert_and_fixup_are_exempt(self):
        for subject in ("Merge branch 'main'", "Revert \"tui: x\"",
                        "fixup! tui: x", "squash! tui: x"):
            self.assertEqual(commit.lint_subjects([subject]), [], subject)

    def test_reports_every_offender_in_order(self):
        problems = commit.lint_subjects(["bad one", "tui: good", "bad two"])
        self.assertEqual([p[0] for p in problems], [0, 2])


class RedSignal(unittest.TestCase):
    def test_a_red_subject_is_found(self):
        self.assertTrue(commit.has_red(["tests: pin the bug (red)"]))

    def test_a_production_change_without_red_warns(self):
        self.assertTrue(commit.tdd_warning(["fix: x"], ["lib/agent.cpp"]))

    def test_a_production_change_with_red_is_quiet(self):
        self.assertFalse(commit.tdd_warning(["tests: pin it (red)", "fix: x"],
                                            ["lib/agent.cpp"]))

    def test_a_docs_change_needs_no_red(self):
        self.assertFalse(commit.tdd_warning(["docs: clarify"], ["docs/a.md"]))


if __name__ == "__main__":
    unittest.main()
