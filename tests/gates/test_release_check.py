"""A tag and a version that disagree is how v0.8.3 became a GitHub release with
no distro package behind it: `release.yml` rewrote `version.txt` from the tag,
`packages.yml` refused to build when the two differed, and only one of those can
be right.

These pin the single owner. The tag has to match the stamped version, no
workflow may write `version.txt` at build time, and both release workflows must
run the identical comparison -- two copies that can drift is how the split
started.
"""

import os
import tempfile
import unittest

from harness import REPO_ROOT, load

release = load("release_check")

RELEASE_YML = os.path.join(REPO_ROOT, ".github", "workflows", "release.yml")
PACKAGES_YML = os.path.join(REPO_ROOT, ".github", "workflows", "packages.yml")

# The comparison both workflows run, verbatim. It reads the tag from a `tag`
# shell variable, which each job sets from its own trigger, so the same line
# works for a tag push and for a `workflow_dispatch`.
ASSERTION = 'test "$(cat version.txt)" = "${tag#v}"'


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class TagShape(unittest.TestCase):
    def test_a_release_tag_loses_its_v(self):
        self.assertEqual(release.normalize_tag("v0.8.4"), "0.8.4")
        self.assertEqual(release.normalize_tag("0.8.4"), "0.8.4")

    def test_a_suffix_is_part_of_the_version(self):
        self.assertEqual(release.normalize_tag("v0.9.0-rc.1"), "0.9.0-rc.1")

    def test_a_branch_is_not_a_release_tag(self):
        """`workflow_dispatch` takes a branch name too, and a branch must never
        resolve to a version."""
        for bad in ("main", "v0.8", "release/0.8.4", "v", ""):
            with self.assertRaises(ValueError, msg=bad):
                release.normalize_tag(bad)


class Comparison(unittest.TestCase):
    def test_matching_versions_are_accepted(self):
        self.assertIsNone(release.compare("0.8.4", "0.8.4"))

    def test_a_mismatch_names_both_versions(self):
        problem = release.compare("0.8.2", "0.8.4")
        self.assertIn("0.8.2", problem)
        self.assertIn("0.8.4", problem)

    def test_the_unstamped_tag_that_shipped_nothing_is_a_mismatch(self):
        """The real case: v0.8.3 was cut on a commit whose version.txt said
        0.8.2, so nothing reached the repositories."""
        self.assertIsNotNone(release.compare("0.8.2", "0.8.3"))


class FormulaVersion(unittest.TestCase):
    def test_the_formula_version_is_read(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "homebrew"))
            with open(os.path.join(root, "homebrew", "amber.rb"), "w", encoding="utf-8") as handle:
                handle.write('  version "1.0.0"\n')
            self.assertEqual(release.read_formula_version(root), "1.0.0")

    def test_a_missing_formula_is_not_a_failure(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(release.read_formula_version(root))

    def test_a_stale_formula_is_a_mismatch(self):
        problem = release.compare_formula("0.7.0", "1.0.0")
        self.assertIn("0.7.0", problem)
        self.assertIn("1.0.0", problem)

    def test_a_matching_formula_is_accepted(self):
        self.assertIsNone(release.compare_formula("1.0.0", "1.0.0"))


class StampedVersion(unittest.TestCase):
    def test_the_version_is_read_from_the_first_line(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "version.txt"), "w", encoding="utf-8") as handle:
                handle.write("0.8.4\n")
            self.assertEqual(release.read_version(root), "0.8.4")

    def test_a_trailing_newline_or_stray_space_does_not_change_the_version(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "version.txt"), "w", encoding="utf-8") as handle:
                handle.write("  0.8.4 \r\n")
            self.assertEqual(release.read_version(root), "0.8.4")

    def test_a_missing_version_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(FileNotFoundError):
                release.read_version(root)


class WorkflowPolicy(unittest.TestCase):
    def test_no_workflow_rewrites_the_version(self):
        """Writing version.txt at build time hides an unstamped tag: the GitHub
        release looks correct while every package channel quietly refuses."""
        for path in (RELEASE_YML, PACKAGES_YML):
            text = read(path)
            for line in text.splitlines():
                if line.lstrip().startswith("#"):
                    continue
                self.assertNotIn("> version.txt", line, f"{path}: {line.strip()}")

    def test_both_release_workflows_run_the_identical_check(self):
        for path in (RELEASE_YML, PACKAGES_YML):
            self.assertIn(ASSERTION, read(path), path)


if __name__ == "__main__":
    unittest.main()
