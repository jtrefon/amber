"""The live-baseline release gate fails closed on every way a release can lie.

A missing artifact, the wrong engine, a non-live run, a short corpus, a failed
scenario, a wrong model, and a regression beyond the recorded band are all
release blockers. A docs/packaging-only tag is the one legitimate skip.
"""

import json
import os
import tempfile
import unittest
from pathlib import Path

from harness import load

gate = load("bench_release_check")


def artifact(engine="0.8.8", model="qwen38-27b-mtp", profile="qwen38-27b-mtp",
             score=800.0, scenarios=49, failed=(), mode="live"):
    rows = []
    for i in range(scenarios):
        row = {"name": f"s-{i}", "suite": "tools", "score": 90.0, "success": True}
        if i in failed:
            row["success"] = False
            row["failures"] = ["scenario failed"]
        rows.append(row)
    return {
        "engine_version": engine,
        "mode": mode,
        "model": model,
        "profile": profile,
        "model_score": score,
        "scenarios": rows,
    }


class Fixture:
    def __init__(self, name):
        self.root = Path(tempfile.mkdtemp(prefix=f"bench_release_{name}_"))

    def write(self, sub, filename, data):
        directory = self.root / sub
        directory.mkdir(parents=True, exist_ok=True)
        (directory / filename).write_text(json.dumps(data), encoding="utf-8")
        return str(directory)

    def write_raw(self, sub, filename, text):
        directory = self.root / sub
        directory.mkdir(parents=True, exist_ok=True)
        (directory / filename).write_text(text, encoding="utf-8")
        return str(directory)

    def cleanup(self):
        import shutil

        shutil.rmtree(self.root, ignore_errors=True)


class ReleaseGate(unittest.TestCase):
    def setUp(self):
        self.f = Fixture(self.id().rsplit(".", 1)[-1])

    def tearDown(self):
        self.f.cleanup()

    def run_check(self, results, **kwargs):
        current = self.f.write("current", "run.json", results)
        kwargs.setdefault("min_scenarios", 2)
        return gate.check(current, "0.8.8", **kwargs)

    def test_check_passes_for_matching_artifact(self):
        report = self.run_check(artifact(scenarios=3))
        self.assertEqual(report.code, 0, "\n".join(report.lines))
        self.assertIn("release baseline green", "\n".join(report.lines))

    def test_check_fails_when_artifact_missing(self):
        report = gate.check(str(self.f.root / "empty"), "0.8.8", min_scenarios=2)
        self.assertEqual(report.code, 1)
        text = "\n".join(report.lines)
        self.assertIn("NOT MEASURED", text)
        self.assertIn("bench/results/releases/0.8.8", text)

    def test_check_fails_on_engine_mismatch(self):
        report = self.run_check(artifact(engine="0.8.7", scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("engine 0.8.7", "\n".join(report.lines))

    def test_check_fails_on_model_mismatch(self):
        report = self.run_check(artifact(model="other", scenarios=3), model="expected")
        self.assertEqual(report.code, 1)
        self.assertIn("model 'other'", "\n".join(report.lines))

    def test_check_fails_on_failed_scenario(self):
        report = self.run_check(artifact(scenarios=4, failed={1}), min_scenarios=2)
        self.assertEqual(report.code, 1)
        self.assertIn("failed scenario", "\n".join(report.lines))

    def test_check_fails_on_regression_beyond_band(self):
        previous = self.f.write("previous", "old.json", artifact(score=850.0, scenarios=3))
        report = self.run_check(artifact(score=800.0, scenarios=3), previous_dir=previous)
        self.assertEqual(report.code, 1)
        self.assertIn("below", "\n".join(report.lines))

    def test_check_accepts_a_drop_inside_the_band(self):
        previous = self.f.write("previous", "old.json", artifact(score=802.0, scenarios=3))
        report = self.run_check(artifact(score=800.0, scenarios=3), previous_dir=previous)
        self.assertEqual(report.code, 0, "\n".join(report.lines))

    def test_check_skips_for_docs_only_tag(self):
        report = self.run_check(
            artifact(scenarios=3),
            changed_paths=["docs/spec/MISSION.md", "website/package.json", ".github/workflows/ci.yml"],
        )
        self.assertEqual(report.code, 0)
        self.assertIn("not required", "\n".join(report.lines))

    def test_unknown_path_is_engine(self):
        self.assertTrue(gate.engine_touching(["new-area/config.json"]))
        self.assertTrue(gate.engine_touching(["completions.json"]))
        self.assertFalse(gate.engine_touching(["docs/spec/MISSION.md"]))
        self.assertFalse(gate.engine_touching(["packaging/debian/control"]))

    def test_check_fails_when_not_live(self):
        report = self.run_check(artifact(mode="hermetic", scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("not 'live'", "\n".join(report.lines))

    def test_check_fails_when_corpus_is_short(self):
        report = self.run_check(artifact(scenarios=2), min_scenarios=5)
        self.assertEqual(report.code, 1)
        self.assertIn("corpus floor is 5", "\n".join(report.lines))

    def test_check_fails_on_unreadable_artifact(self):
        directory = self.f.write_raw("current", "broken.json", "{ not json")
        report = gate.check(directory, "0.8.8", min_scenarios=2)
        self.assertEqual(report.code, 1)
        self.assertIn("unreadable", "\n".join(report.lines))

    def test_corpus_counts_only_real_scenarios(self):
        corpus = self.f.root / "corpus" / "suite"
        corpus.mkdir(parents=True)
        (corpus / "one.json").write_text(json.dumps({"name": "one", "prompt": "p"}))
        (corpus / "two.json").write_text(json.dumps({"name": "two", "prompt": "p"}))
        (corpus / "not-a-scenario.json").write_text(json.dumps({"note": "x"}))
        self.assertEqual(gate.corpus_count(str(self.f.root / "corpus")), 2)


if __name__ == "__main__":
    unittest.main()
