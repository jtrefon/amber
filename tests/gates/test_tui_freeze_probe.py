"""The #621 macOS probe: dispatch-only, repeats the detector, samples on failure.

The freeze is rare and macOS-specific; the probe is how a failing occurrence
becomes a stack instead of a screen dump. It is a measurement tool, not a gate:
if it ever joins `ci.yml`, the decision to measure #621 has changed and the
docs and the issue need to say so.
"""

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class Probe(unittest.TestCase):
    def workflow(self):
        return (REPO_ROOT / ".github/workflows/tui-freeze-probe.yml").read_text(
            encoding="utf-8")

    def test_the_probe_is_a_dispatch_only_macos_repeat(self):
        text = self.workflow()
        self.assertIn("workflow_dispatch:", text)
        self.assertIn("runs-on: macos-latest", text)
        self.assertIn("AMBER_TUI_JOURNEY: tab-switch-live", text)
        self.assertIn("--repeat", text)
        # BUILD_DIR is absolute; a relative goal silently falls back to an
        # existing binary on a built host and fails in a fresh checkout.
        self.assertIn('"$PWD/amber" "$PWD/tui_agent_e2e_test"', text)
        self.assertNotIn("schedule:", text)

    def test_the_probe_is_not_in_the_gating_workflow(self):
        ci = (REPO_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertNotIn("tui-freeze-probe", ci)

    def test_a_failed_assertion_samples_the_live_session(self):
        harness = (REPO_ROOT / "tests/pty_harness.h").read_text(encoding="utf-8")
        impl = (REPO_ROOT / "tests/pty_harness.cpp").read_text(encoding="utf-8")
        self.assertIn("sample_for_diagnosis", harness)
        self.assertIn("tui.sample_for_diagnosis()", impl)


if __name__ == "__main__":
    unittest.main()
