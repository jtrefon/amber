"""Inventory counts in the docs must match the tree.

Three counts drifted in the 0.9 cycle: the TUI journeys (13/17 vs 27), the
hermetic baseline (46 + 25 additive vs 48, no additive) and the fuzz targets
(four vs five). They are release claims, so a graduation, a new journey or a
new fuzz target has to update the prose in the same change.
"""

import json
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORDS = {1: "One", 2: "Two", 3: "Three", 4: "Four", 5: "Five", 6: "Six",
         7: "Seven", 8: "Eight"}


def read(path):
    return (REPO_ROOT / path).read_text(encoding="utf-8")


class DocsTruth(unittest.TestCase):
    def test_the_journey_count_matches_the_inventory(self):
        inventory = read("docs/spec/tui/e2e-testing.md")
        match = re.search(r"The (\d+) journeys are:", inventory)
        self.assertIsNotNone(match)
        count = match.group(1)
        self.assertIn(f"| {count} journeys |",
                      read("docs/spec/benchmark/hermetic-corpus.md"))
        self.assertIn(f"({count} journeys)", read("docs/platform-support.md"))

    def test_the_hermetic_cell_count_matches_the_baseline(self):
        baseline = json.loads(read("bench/results/harness-efficiency-baseline.json"))
        count = len(baseline["scenarios"])
        self.assertRegex(read("docs/spec/benchmark/hermetic-corpus.md"),
                         rf"\b{count} gated cells")
        self.assertRegex(read("BENCHMARK.md"), rf"\b{count} cells")
        self.assertNotIn("additive cells, reported separately", read("BENCHMARK.md"))

    def test_the_fuzz_target_count_matches_the_sources(self):
        targets = len(list((REPO_ROOT / "fuzz").glob("*.cpp")))
        self.assertIn(targets, WORDS, "extend the word list for the new count")
        self.assertIn(f"{WORDS[targets]} targets", read("AGENTS.md"))


if __name__ == "__main__":
    unittest.main()
