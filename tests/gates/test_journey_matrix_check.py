"""The 0.9 journey-matrix gate fails closed on every way coverage can lie.

A missing row, an unknown or duplicated source, a journey the runner cannot
execute, a placeholder reason, a mismatched surface list, an alias split from
its target and a degraded scan are all findings. A deferred cell is a recorded
gap: allowed while the work is open, refused by `--require-complete`.

The fixture is synthetic and meets the real source floors (170 commands, 60
capabilities, 18 CLI journeys, 14 TUI selectors, 5 long-horizon scenarios, 30
harness scenarios), so a test cannot pass by shrinking the product.
"""

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from harness import load

checker = load("journey_matrix_check")
journey_sources = load("journey_sources")

COMMANDS = 170
CAPABILITIES = 60
DEFAULT_TOOLS = 10
CLI_JOURNEYS = 18
TUI_JOURNEYS = 13  # plus the unconditional default journey
LONG_HORIZON = 5
HARNESS = 30
UI_ACTIONS = 5


class Fixture:
    def __init__(self, name):
        self.root = Path(tempfile.mkdtemp(prefix=f"journey_matrix_{name}_"))

    def write_json(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def write_text(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def seed(self):
        commands = {}
        for i in range(COMMANDS):
            commands[f"c{i:03d}"] = {"action": f"core.c{i:03d}", "help": f"command {i}"}
        self.write_json(
            "completions.json",
            {"commands": commands, "aliases": {"alias": "c000"}, "legacy": {}},
        )

        capabilities = []
        for i in range(CAPABILITIES):
            capabilities.append(f"capability\tp{i:03d}\ttool\tt{i:03d}")
        for i in range(DEFAULT_TOOLS):
            capabilities.append(f"default-tool\tt{i:03d}")
        self.write_text(
            "tests/capability_snapshot.txt",
            "# fixture\n" + "\n".join(capabilities) + "\n",
        )

        menus = {"menus": [{"label": "View", "items": [
            {"label": f"Action {i}", "action": f"ui.fixture{i}"} for i in range(UI_ACTIONS)
        ]}]}
        self.write_json("menus.json", menus)
        self.write_json("keybindings.json", {"version": 2, "bindings": []})

        cli_rows = [{"name": f"cli-{i:02d}", "ok": True} for i in range(CLI_JOURNEYS)]
        self.write_json("bench/results/agent-e2e-baseline.json", {"rows": cli_rows})
        scenario_dir = self.root / "bench/scenarios/e2e"
        scenario_dir.mkdir(parents=True)
        for i in range(CLI_JOURNEYS):
            self.write_json(f"bench/scenarios/e2e/cli-{i:02d}.json", {"name": f"cli-{i:02d}"})

        self.write_json(
            "bench/results/long-horizon-baseline.json",
            {"scenarios": {f"long-{i}": {} for i in range(LONG_HORIZON)}},
        )
        self.write_json(
            "bench/results/harness-efficiency-baseline.json",
            {"scenarios": {f"bench-{i:02d}": {} for i in range(HARNESS)}},
        )

        selectors = "\n".join(
            f'if (!only || std::string(only) == "tui-{i:02d}") journey_x(binary);'
            for i in range(TUI_JOURNEYS)
        )
        self.write_text(
            "tests/tui_agent_e2e_test.cpp",
            "void journey_read_edit_verify(const std::string&) {}\n" + selectors + "\n",
        )

    def sources(self):
        return journey_sources.source_map(self.root)

    def manifest(self):
        sources = self.sources()
        rows = []
        aliases = {}
        for index, ref in enumerate(sorted(sources)):
            if ref.startswith("alias:"):
                aliases[ref] = index
                continue
            kind = sources[ref].kind
            cli = "cli-00"
            tui = "tui-00"
            bench = "bench-00"
            if kind == "capability":
                cli = "internal: fixture capability"
                tui = "internal: fixture capability"
            elif kind == "default-tool":
                tui = "internal: fixture default tool"
                bench = "internal: fixture default tool"
            elif kind == "ui-action":
                cli = "na: fixture has no CLI surface"
                bench = "na: fixture has no bench surface"
            elif kind == "surface":
                cli = "na: fixture has no CLI surface"
                bench = "na: fixture has no bench surface"
            rows.append(
                {
                    "id": f"r{len(rows):03d}",
                    "sources": [ref],
                    "journeys": {"cli": cli, "tui": tui, "bench": bench},
                    "notes": "fixture",
                }
            )
        # An alias shares its target's row.
        for alias, _ in aliases.items():
            target = sources[alias].detail["target"].replace(" ", ".")
            owner = next(r for r in rows if f"command:{target}" in r["sources"])
            owner["sources"].append(alias)
        return {
            "version": 1,
            "claim": "fixture",
            "alias_owners": {},
            "sources": {"commands": "completions.json"},
            "surfaces": [
                {"id": name, "source": "fixture", "description": "fixture"}
                for name in journey_sources.SURFACES
            ],
            "capabilities": rows,
        }

    def write_manifest(self, manifest):
        self.write_json("tests/journey_matrix.json", manifest)

    def load_manifest(self):
        return json.loads((self.root / "tests/journey_matrix.json").read_text(encoding="utf-8"))

    def run_check(self, require_complete=False):
        return checker.check(str(self.root), require_complete=require_complete)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


class MatrixGate(unittest.TestCase):
    def setUp(self):
        self.f = Fixture(self.id().rsplit(".", 1)[-1])
        self.f.seed()
        self.f.write_manifest(self.f.manifest())

    def tearDown(self):
        self.f.cleanup()

    def check(self, require_complete=False):
        report = self.f.run_check(require_complete=require_complete)
        return report.code, "\n".join(report.lines)

    def test_complete_matrix_passes(self):
        code, text = self.check()
        self.assertEqual(code, 0, text)
        self.assertIn("journey matrix green", text)

    def test_missing_row_is_a_finding(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"].pop()
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("in no capability row", text)

    def test_unknown_source_is_a_finding(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"][0]["sources"].append("command:not-a-command")
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("unknown source", text)

    def test_duplicate_source_is_a_finding(self):
        manifest = self.f.load_manifest()
        duplicate = manifest["capabilities"][0]["sources"][0]
        manifest["capabilities"][1]["sources"].append(duplicate)
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("is in both", text)

    def test_unknown_journey_is_a_finding(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"][0]["journeys"]["cli"] = "cli-not-a-journey"
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("unknown cli journey", text)

    def test_placeholder_reason_is_a_finding(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"][0]["journeys"]["tui"] = "internal: x"
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("needs a reason", text)

    def test_deferred_is_allowed_then_refused(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"][0]["journeys"]["bench"] = "deferred: #709 pending work"
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 0, text)
        code, text = self.check(require_complete=True)
        self.assertEqual(code, 1, text)
        self.assertIn("deferred gap remains", text)

    def test_bad_deferred_format_is_a_finding(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"][0]["journeys"]["bench"] = "deferred: pending work"
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("#issue", text)

    def test_surface_list_must_match(self):
        manifest = self.f.load_manifest()
        manifest["surfaces"] = manifest["surfaces"][:-1]
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 2, text)
        self.assertIn("surfaces do not match", text)

    def test_alias_must_share_its_target_row(self):
        manifest = self.f.load_manifest()
        alias_row = next(r for r in manifest["capabilities"] if any(
            s.startswith("alias:") for s in r["sources"]))
        alias = next(s for s in alias_row["sources"] if s.startswith("alias:"))
        alias_row["sources"].remove(alias)
        manifest["capabilities"][-1]["sources"].append(alias)
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("its target", text)

    def test_alias_owner_exception_is_explicit(self):
        manifest = self.f.load_manifest()
        alias_row = next(r for r in manifest["capabilities"] if any(
            s.startswith("alias:") for s in r["sources"]))
        alias = next(s for s in alias_row["sources"] if s.startswith("alias:"))
        alias_row["sources"].remove(alias)
        target = manifest["capabilities"][-1]
        target["sources"].append(alias)
        manifest["alias_owners"] = {alias.split(":", 1)[1]: target["id"]}
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 0, text)

    def test_empty_row_list_fails_closed(self):
        manifest = self.f.load_manifest()
        manifest["capabilities"] = []
        self.f.write_manifest(manifest)
        code, text = self.check()
        self.assertEqual(code, 2, text)
        self.assertIn("rows", text)

    def test_degraded_command_scan_fails_closed(self):
        (self.f.root / "completions.json").write_text(
            json.dumps({"commands": {"only": {"action": "core.only"}}, "aliases": {}}),
            encoding="utf-8",
        )
        code, text = self.check()
        self.assertEqual(code, 2, text)
        self.assertIn("source scan failed", text)

    def test_missing_snapshot_fails_closed(self):
        (self.f.root / "tests/capability_snapshot.txt").unlink()
        code, text = self.check()
        self.assertEqual(code, 2, text)
        self.assertIn("source scan failed", text)

    def test_missing_manifest_is_an_error(self):
        (self.f.root / "tests/journey_matrix.json").unlink()
        code, text = self.check()
        self.assertEqual(code, 2, text)
        self.assertIn("cannot read", text)


if __name__ == "__main__":
    unittest.main()
