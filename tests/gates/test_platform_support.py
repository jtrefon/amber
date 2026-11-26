"""The platform matrix must match what CI actually gates.

The matrix is a support claim, so a stale count or a terminal row promoted to
"gated" without a CI step is a documentation bug. tmux and GNU screen are the
two multiplexer cells that used to be manual; `make terminal-smoke` is the one
command that exercises both.
"""

import os
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (REPO_ROOT / path).read_text(encoding="utf-8")


class PlatformMatrix(unittest.TestCase):
    def test_the_journey_count_matches_the_inventory(self):
        spec = read("docs/spec/tui/e2e-testing.md")
        match = re.search(r"The (\d+) journeys are:", spec)
        self.assertIsNotNone(match, "the journey inventory lost its count line")
        matrix = read("docs/platform-support.md")
        self.assertIn(f"({match.group(1)} journeys)", matrix)

    def test_tmux_and_screen_are_gated_by_one_command(self):
        matrix = read("docs/platform-support.md")
        self.assertIn("| tmux |", matrix)
        self.assertIn("Gated (macOS and Linux)", matrix)
        self.assertIn("| GNU screen |", matrix)
        self.assertIn("Gated (Linux)", matrix)
        self.assertIn("make terminal-smoke", matrix)

    def test_the_linux_tui_job_runs_the_terminal_smoke(self):
        ci = read(".github/workflows/ci.yml")
        start = ci.index("  tui-agent-e2e:")
        end = ci.index("\n  coverage:", start)
        job = ci[start:end]
        self.assertIn("make terminal-smoke", job)
        self.assertIn("extra: tmux screen", job)

    def test_the_makefile_and_scripts_exist(self):
        makefile = read("Makefile.in")
        self.assertIn("terminal-smoke:", makefile)
        for script in ("tests/tmux_smoke.sh", "tests/screen_smoke.sh"):
            path = REPO_ROOT / script
            self.assertTrue(path.is_file(), f"{script} is missing")
            self.assertTrue(os.access(path, os.X_OK), f"{script} is not executable")

    def test_621_is_recorded_as_a_deferral_not_a_gated_cell(self):
        matrix = " ".join(read("docs/platform-support.md").split())
        self.assertIn("#621", matrix)
        self.assertIn("recorded 0.9 deferral", matrix)
        self.assertIn("known_unmeasured", matrix)


if __name__ == "__main__":
    unittest.main()
