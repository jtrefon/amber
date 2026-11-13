"""The action-pin gate: a mutable tag in a release workflow is a supply-chain risk."""

import os
import tempfile
import unittest
from pathlib import Path

from harness import load

pins = load("action_pins")

PIN = "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7"


class Pinning(unittest.TestCase):
    def scan(self, workflow):
        with tempfile.TemporaryDirectory() as root:
            target = Path(root, ".github", "workflows")
            target.mkdir(parents=True)
            target.joinpath("ci.yml").write_text(workflow, encoding="utf-8")
            return pins.scan(root)

    def test_the_real_tree_is_pinned(self):
        self.assertEqual(pins.scan(), [])

    def test_a_mutable_tag_fails(self):
        found = self.scan("steps:\n  - uses: actions/checkout@v7\n")
        self.assertEqual(len(found), 1)
        self.assertIn("commit SHA", found[0][3])

    def test_a_sha_without_the_tag_comment_fails(self):
        found = self.scan("steps:\n  - uses: actions/checkout@"
                          "3d3c42e5aac5ba805825da76410c181273ba90b1\n")
        self.assertEqual(len(found), 1)
        self.assertIn("tag comment", found[0][3])

    def test_a_sha_with_the_tag_comment_passes(self):
        self.assertEqual(self.scan(f"steps:\n  - uses: {PIN}\n"), [])

    def test_a_local_composite_and_a_docker_reference_are_not_pinned(self):
        workflow = ("steps:\n  - uses: ./.github/actions/install-deps\n"
                    "  - uses: docker://alpine:3.20\n")
        self.assertEqual(self.scan(workflow), [])

    def test_composite_action_manifests_are_scanned_too(self):
        with tempfile.TemporaryDirectory() as root:
            target = Path(root, ".github", "actions", "x")
            target.mkdir(parents=True)
            target.joinpath("action.yml").write_text(
                "runs:\n  using: composite\n  steps:\n"
                "    - uses: actions/cache@v6\n", encoding="utf-8")
            found = pins.scan(root)
        self.assertEqual(len(found), 1)
        self.assertIn("actions/cache@v6", found[0][2])


if __name__ == "__main__":
    unittest.main()
