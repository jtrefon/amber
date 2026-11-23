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
             score=800.0, scenarios=49, failed=(), mode="live", token_scale=1.0,
             calls=1, wall_ms=1000, ttft_ms=100, compaction=0, failure="scenario failed",
             row_score=90.0):
    rows = []
    for i in range(scenarios):
        prompt = int(100 * token_scale)
        completion = int(10 * token_scale)
        row = {
            "name": f"s-{i}", "suite": "tools", "score": row_score, "success": True,
            "prompt_tokens": prompt, "completion_tokens": completion,
            "repeat_prompt_tokens": [prompt] * 3,
            "repeat_completion_tokens": [completion] * 3,
            "tool_calls_total": calls, "tool_calls": [{}] * calls,
            "wall_ms": wall_ms, "ttft_ms": ttft_ms,
            "compaction_failures": compaction,
        }
        if i in failed:
            row["success"] = False
            row["failures"] = [failure]
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


class CostBands(unittest.TestCase):
    def setUp(self):
        self.f = Fixture(self.id().rsplit(".", 1)[-1])

    def tearDown(self):
        self.f.cleanup()

    def run_check(self, current, previous, **kwargs):
        current_dir = self.f.write("current", "run.json", current)
        previous_dir = self.f.write("previous", "old.json", previous)
        kwargs.setdefault("min_scenarios", 2)
        kwargs.setdefault("comparable_floor", 2)
        return gate.check(current_dir, "0.8.8", previous_dir=previous_dir, **kwargs)

    def test_equal_cost_passes(self):
        report = self.run_check(artifact(scenarios=3), artifact(scenarios=3))
        self.assertEqual(report.code, 0, "\n".join(report.lines))
        self.assertIn("cost 3 common", "\n".join(report.lines))

    def test_token_regression_beyond_ten_percent_fails(self):
        report = self.run_check(artifact(scenarios=3, token_scale=1.2), artifact(scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("tokens up", "\n".join(report.lines))

    def test_token_growth_inside_the_band_passes(self):
        report = self.run_check(artifact(scenarios=3, token_scale=1.05), artifact(scenarios=3))
        self.assertEqual(report.code, 0, "\n".join(report.lines))

    def test_tool_call_regression_beyond_ten_percent_fails(self):
        report = self.run_check(artifact(scenarios=3, calls=2), artifact(scenarios=3, calls=1))
        self.assertEqual(report.code, 1)
        self.assertIn("tool calls up", "\n".join(report.lines))

    def test_failed_scenarios_are_excluded_from_cost(self):
        previous = artifact(scenarios=3)
        current = artifact(scenarios=3)
        current["scenarios"][0]["success"] = False
        current["scenarios"][0]["failures"] = ["final answer failed scenario checks"]
        current["scenarios"][0]["prompt_tokens"] = 10**9
        current["scenarios"][0]["repeat_prompt_tokens"] = [10**9] * 3
        report = self.run_check(current, previous)
        text = "\n".join(report.lines)
        self.assertNotIn("tokens up", text)

    def test_comparable_floor_fails_closed(self):
        report = self.run_check(artifact(scenarios=3), artifact(scenarios=3), comparable_floor=4)
        self.assertEqual(report.code, 1)
        self.assertIn("comparable successful", "\n".join(report.lines))

    def test_compaction_failure_blocks(self):
        report = self.run_check(artifact(scenarios=3, compaction=1), artifact(scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("compaction failure", "\n".join(report.lines))

    def test_hard_stop_blocks(self):
        report = self.run_check(
            artifact(scenarios=3, failed={0}, failure="hard stop: wall budget"),
            artifact(scenarios=3),
        )
        self.assertEqual(report.code, 1)
        self.assertIn("hard stop", "\n".join(report.lines))

    def test_wall_pathology_blocks(self):
        report = self.run_check(artifact(scenarios=3, wall_ms=100000), artifact(scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("wall", "\n".join(report.lines))

    def test_ttft_pathology_blocks(self):
        report = self.run_check(artifact(scenarios=3, ttft_ms=50000), artifact(scenarios=3))
        self.assertEqual(report.code, 1)
        self.assertIn("TTFT", "\n".join(report.lines))

    def test_corpus_change_uses_the_common_score(self):
        report = self.run_check(artifact(scenarios=4, score=700), artifact(scenarios=3, score=800))
        text = "\n".join(report.lines)
        self.assertEqual(report.code, 0, text)
        self.assertIn("corpus changed", text)
        self.assertIn("common score", text)

    def test_common_score_regression_fails(self):
        report = self.run_check(
            artifact(scenarios=4, score=800, row_score=80),
            artifact(scenarios=3, score=800),
        )
        self.assertEqual(report.code, 1)
        self.assertIn("common-corpus score", "\n".join(report.lines))

    def test_no_previous_notes_cost_bands_not_measured(self):
        directory = self.f.write("current", "run.json", artifact(scenarios=3))
        report = gate.check(directory, "0.8.8", min_scenarios=2)
        self.assertEqual(report.code, 0, "\n".join(report.lines))
        self.assertIn("cost bands not measured", "\n".join(report.lines))


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

    def test_check_fails_on_unexpected_scenario_failure(self):
        report = self.run_check(artifact(scenarios=4, failed={1}), min_scenarios=2)
        self.assertEqual(report.code, 1)
        self.assertIn("unexpected scenario failure", "\n".join(report.lines))

    def test_check_allows_a_model_quality_failure(self):
        data = artifact(scenarios=3)
        data["scenarios"][1]["success"] = False
        data["scenarios"][1]["failures"] = ["oracle not matched: 1/2 steps"]
        report = self.run_check(data)
        self.assertEqual(report.code, 0, "\n".join(report.lines))

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
        self.assertTrue(gate.engine_touching([]))
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

    def test_discover_previous_picks_the_highest_earlier_release(self):
        for version in ("0.8.8", "0.8.9", "0.9.0", "0.10.0"):
            self.f.write(f"releases/{version}", "run.json", {"model_score": 1})
        (self.f.root / "releases" / "not-a-version").mkdir()
        self.assertEqual(
            Path(gate.discover_previous(str(self.f.root / "releases/0.9.0"))).name, "0.8.9"
        )
        self.assertEqual(
            Path(gate.discover_previous(str(self.f.root / "releases/0.8.9"))).name, "0.8.8"
        )
        self.assertIsNone(gate.discover_previous(str(self.f.root / "releases/0.8.8")))
        self.assertIsNone(gate.discover_previous(str(self.f.root / "elsewhere")))

    def test_corpus_counts_only_real_scenarios(self):
        corpus = self.f.root / "corpus" / "suite"
        corpus.mkdir(parents=True)
        (corpus / "one.json").write_text(json.dumps({"name": "one", "prompt": "p"}))
        (corpus / "two.json").write_text(json.dumps({"name": "two", "prompt": "p"}))
        (corpus / "not-a-scenario.json").write_text(json.dumps({"note": "x"}))
        (corpus / "probe.json").write_text(
            json.dumps({"name": "probe", "prompt": "p", "hermetic_only": True})
        )
        self.assertEqual(gate.corpus_count(str(self.f.root / "corpus")), 2)


if __name__ == "__main__":
    unittest.main()
