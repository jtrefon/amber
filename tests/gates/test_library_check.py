"""The library check must run what it says, fail when a block fails, and skip
only what is marked.

An entry's `run` blocks are the difference between verified knowledge and prose.
If the check silently skips a block -- or passes when a command fails -- the
entries rot without anyone noticing, which is exactly the failure the block
exists to prevent.
"""

import os
import shutil
import tempfile
import unittest

from harness import load

lc = load("library_check")


class LibraryCheckTests(unittest.TestCase):
    def _library(self, body):
        root = tempfile.mkdtemp(prefix="library_check_test_")
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        directory = os.path.join(root, "prompts", "domains", "coding", "library")
        os.makedirs(directory)
        with open(os.path.join(directory, "entry.md"), "w", encoding="utf-8") as handle:
            handle.write(body)
        return directory

    def test_a_passing_block_passes(self):
        directory = self._library("trigger: t\n\n## Run it\n\n```run\nexit 0\n```\n")
        self.assertEqual(lc.main(["--dir", directory]), 0)

    def test_a_failing_block_fails(self):
        directory = self._library("trigger: t\n\n## Run it\n\n```run\nexit 3\n```\n")
        self.assertEqual(lc.main(["--dir", directory]), 1)

    def test_a_failing_block_reports_its_output(self):
        directory = self._library("trigger: t\n\n## Run it\n\n```run\necho boom >&2\nexit 1\n```\n")
        self.assertEqual(lc.main(["--dir", directory]), 1)

    def test_a_manual_block_is_skipped(self):
        directory = self._library(
            "trigger: t\n\n## Run it\n\n```run\n# manual: needs a cluster\nexit 9\n```\n")
        self.assertEqual(lc.main(["--dir", directory]), 0)

    def test_a_missing_library_fails_closed(self):
        missing = os.path.join(tempfile.gettempdir(), "library_check_absent_dir")
        self.assertEqual(lc.main(["--dir", missing]), 2)

    def test_blocks_are_parsed_with_their_lines(self):
        directory = self._library("trigger: t\n\n## Run it\n\n```run\necho one\n```\n")
        entries = lc.entry_files(directory)
        self.assertEqual(len(entries), 1)
        blocks = lc.run_blocks(entries[0])
        self.assertEqual(len(blocks), 1)
        self.assertIn("echo one", blocks[0][1])

    def test_the_vocabulary_is_not_an_entry(self):
        directory = self._library("trigger: t\n")
        with open(os.path.join(directory, "tags.md"), "w", encoding="utf-8") as handle:
            handle.write("- `cpp` — a tag\n")
        self.assertEqual(len(lc.entry_files(directory)), 1)
