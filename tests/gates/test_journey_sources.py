"""The source scan is the matrix's oracle, so it fails closed on every floor.

A scan that loses `completions.json`, the capability snapshot, the TUI selector
lines or a journey baseline must raise, not return a smaller list: a smaller
list is exactly what makes the matrix look complete.
"""

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from harness import load

sources = load("journey_sources")


class Fixture:
    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="journey_sources_"))

    def write_json(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def write_text(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


class Floors(unittest.TestCase):
    def setUp(self):
        self.f = Fixture()

    def tearDown(self):
        self.f.cleanup()

    def test_missing_completions_is_a_scan_error(self):
        with self.assertRaises(sources.ScanError):
            sources.completion_paths(self.f.root)

    def test_too_few_commands_is_a_scan_error(self):
        self.f.write_json(
            "completions.json",
            {"commands": {"only": {"action": "core.only"}}, "aliases": {}},
        )
        with self.assertRaises(sources.ScanError):
            sources.completion_paths(self.f.root)

    def test_missing_snapshot_is_a_scan_error(self):
        with self.assertRaises(sources.ScanError):
            sources.snapshot_capabilities(self.f.root)

    def test_too_few_capabilities_is_a_scan_error(self):
        self.f.write_text("tests/capability_snapshot.txt", "capability\tp\ttool\tt\n")
        with self.assertRaises(sources.ScanError):
            sources.snapshot_capabilities(self.f.root)

    def test_too_few_default_tools_is_a_scan_error(self):
        self.f.write_text("tests/capability_snapshot.txt", "default-tool\tread\n")
        with self.assertRaises(sources.ScanError):
            sources.default_tools(self.f.root)

    def test_no_ui_actions_is_a_scan_error(self):
        self.f.write_json(
            "completions.json",
            {
                "commands": {
                    f"c{i}": {"action": f"core.c{i}"} for i in range(sources.MIN_COMMANDS)
                },
                "aliases": {},
            },
        )
        self.f.write_json("menus.json", {"menus": []})
        self.f.write_json("keybindings.json", {"bindings": []})
        with self.assertRaises(sources.ScanError):
            sources.ui_actions(self.f.root)

    def test_too_few_tui_selectors_is_a_scan_error(self):
        self.f.write_text(
            "tests/tui_agent_e2e_test.cpp",
            "void journey_read_edit_verify(const std::string&) {}\n",
        )
        with self.assertRaises(sources.ScanError):
            sources.tui_journeys(self.f.root)

    def test_missing_cli_baseline_is_a_scan_error(self):
        with self.assertRaises(sources.ScanError):
            sources.cli_journeys(self.f.root)

    def test_too_few_long_horizon_scenarios_is_a_scan_error(self):
        self.f.write_json("bench/results/long-horizon-baseline.json", {"scenarios": {"one": {}}})
        with self.assertRaises(sources.ScanError):
            sources.long_horizon_journeys(self.f.root)

    def test_too_few_harness_scenarios_is_a_scan_error(self):
        self.f.write_json(
            "bench/results/harness-efficiency-baseline.json", {"scenarios": {"one": {}}}
        )
        self.f.write_json(
            "bench/results/long-horizon-baseline.json",
            {"scenarios": {f"long-{i}": {} for i in range(sources.MIN_LONG_HORIZON)}},
        )
        with self.assertRaises(sources.ScanError):
            sources.bench_journeys(self.f.root)

    def test_surfaces_are_the_fixed_taxonomy(self):
        self.assertEqual(
            set(sources.surface_sources()),
            {f"surface:{name}" for name in sources.SURFACES},
        )
        self.assertIn("status-bar", sources.SURFACES)


if __name__ == "__main__":
    unittest.main()
