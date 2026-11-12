"""Pin the runner image the same way the analyzers are pinned.

`ubuntu-latest` moved from 22.04 to 24.04 and is scheduled to move to 26.04.
A build that depends on the image therefore changes without a commit: a
compiler, ncurses or apt behaviour can shift between two green runs on the same
SHA. The workflows pin the image instead, and this test fails when a new job
uses a floating label -- or a different image -- so the choice stays explicit.
"""

import re
import unittest
from pathlib import Path

from harness import REPO_ROOT

WORKFLOWS = Path(REPO_ROOT) / ".github" / "workflows"
RUNS_ON = re.compile(r"^\s*runs-on:\s*(\S+)\s*$", re.MULTILINE)
ALLOWED = ("ubuntu-24.04", "macos-latest")


class RunnerPins(unittest.TestCase):
    def workflows(self):
        paths = sorted(WORKFLOWS.glob("*.yml"))
        self.assertGreaterEqual(len(paths), 6, "workflow scan found too few files")
        return paths

    def test_no_workflow_uses_a_floating_ubuntu_label(self):
        offenders = []
        for path in self.workflows():
            for match in RUNS_ON.finditer(path.read_text(encoding="utf-8")):
                label = match.group(1)
                if label.startswith("ubuntu-") and label != "ubuntu-24.04":
                    offenders.append(f"{path.name}: {label}")
        self.assertEqual(offenders, [], "pin Linux jobs to ubuntu-24.04")

    def test_every_literal_runner_is_a_supported_pin(self):
        found = 0
        unknown = []
        for path in self.workflows():
            for match in RUNS_ON.finditer(path.read_text(encoding="utf-8")):
                label = match.group(1)
                if label.startswith("${{"):
                    continue
                found += 1
                if label not in ALLOWED:
                    unknown.append(f"{path.name}: {label}")
        self.assertGreaterEqual(found, 20, "runs-on scan found too few jobs")
        self.assertEqual(
            unknown,
            [],
            f"runner image must be one of {ALLOWED}; add a deliberate pin here if "
            "the job genuinely needs a different one",
        )

    def test_the_upgrade_path_is_documented(self):
        text = (Path(REPO_ROOT) / "AGENTS.md").read_text(encoding="utf-8")
        self.assertIn("ubuntu-24.04", text)
        self.assertIn("mirror-probe", text)


if __name__ == "__main__":
    unittest.main()
