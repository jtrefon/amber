"""The #703 mutation decision: advisory, outside ci-gate, decision recorded.

Mutation testing is deliberately not a gate (see
`docs/plan/mutation-testing-spike.md`). These checks make the decision
enforceable: a future change that adds a mutation job to `ci.yml` or the gate
inventory, or deletes the measurement, fails here and has to update the
decision instead of silently ratcheting a score.
"""

import re
import unittest
from pathlib import Path

from harness import load

REPO_ROOT = Path(__file__).resolve().parents[2]
manifest = load("gates_manifest")


class Policy(unittest.TestCase):
    def test_the_mutation_workflow_is_separate_and_advisory(self):
        workflow = (REPO_ROOT / ".github/workflows/mutation.yml").read_text(
            encoding="utf-8")
        self.assertIn("Mutation (advisory)", workflow)
        self.assertIn("schedule:", workflow)

    def test_ci_has_no_mutation_job_and_the_inventory_excludes_it(self):
        ci = (REPO_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"^  mutation:", ci, re.MULTILINE))
        self.assertNotIn("mutation", manifest.JOBS)

    def test_the_decision_and_measurements_are_recorded(self):
        text = (REPO_ROOT / "docs/plan/mutation-testing-spike.md").read_text(
            encoding="utf-8")
        flat = " ".join(text.split())
        self.assertIn("Final 0.9 decision", text)
        self.assertIn("reject a per-module or changed-line survivor budget", flat)
        self.assertIn("43/43", text)
        self.assertIn("14/14", text)
        self.assertIn("37%", text)
        self.assertIn("91%", text)
        self.assertIn("equivalent", text)

    def test_the_plan_and_docs_name_the_advisory_set(self):
        agents = (REPO_ROOT / "AGENTS.md").read_text(encoding="utf-8")
        ci_local = (REPO_ROOT / "docs/ci-local.md").read_text(encoding="utf-8")
        plan = (REPO_ROOT / "docs/plan/0.9-readiness.md").read_text(encoding="utf-8")
        self.assertIn("Mutation testing is advisory", agents)
        self.assertIn("Mutation (advisory", ci_local)
        self.assertIn("decided advisory", plan)


if __name__ == "__main__":
    unittest.main()
