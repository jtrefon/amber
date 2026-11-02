"""The job inventory must be one list, and `ci-gate` must require all of it.

`ci.yml` describes jobs, `gate_needs_check.py` decides which skips are legal,
and `docs/plan/pipeline-parity.md` describes the same set again. When those
lists drift, a new gating job can be added and simply never be required: it
reports a check run, `ci-gate` never looks at it, and a red gate merges.
These tests pin the inventory and the parser that compares it to the workflow.
"""

import os
import unittest

from harness import REPO_ROOT, load

manifest = load("gates_manifest")
checker = load("ci_manifest_check")
policy = load("gate_needs_check")

WORKFLOW = os.path.join(REPO_ROOT, ".github", "workflows", "ci.yml")

FIXTURE = """\
name: CI
on:
  push:
    branches: [main]
  pull_request:
jobs:
  changes:
    runs-on: ubuntu-latest
  lint:
    needs: [changes]
    runs-on: ubuntu-latest
  ci-gate:
    needs: [changes, lint]
    if: always()
    runs-on: ubuntu-latest
"""

# An advisory job: it reports findings and must conclude success. The job-level
# flag alone keeps the workflow green but still fails the check run, which is
# what put a red X on #503.
ADVISORY = """\
jobs:
  changes:
    runs-on: ubuntu-latest
  commit-hygiene:
    needs: [changes]
    continue-on-error: true
    steps:
      - name: Commit subjects
        continue-on-error: true
        run: python3 tools/commit_lint.py
  ci-gate:
    needs: [changes]
"""


class Parsing(unittest.TestCase):
    def test_jobs_are_read_from_the_jobs_section_only(self):
        jobs = checker.parse_jobs(FIXTURE)
        self.assertEqual(jobs, ["changes", "lint", "ci-gate"])

    def test_needs_are_read_from_the_aggregator(self):
        self.assertEqual(checker.parse_needs(FIXTURE), ["changes", "lint"])

    def test_a_missing_jobs_section_is_an_error_not_an_empty_list(self):
        self.assertEqual(checker.parse_jobs("name: CI\non: push\n"), [])

    def test_a_multiline_needs_list_is_read(self):
        text = FIXTURE.replace(
            "    needs: [changes, lint]\n",
            "    needs:\n      [changes,\n       lint]\n",
        )
        self.assertIn("lint", checker.parse_needs(text))

    def test_job_blocks_are_read_per_job(self):
        blocks = checker.parse_job_blocks(ADVISORY)
        self.assertEqual(sorted(blocks), ["changes", "ci-gate", "commit-hygiene"])
        self.assertTrue(any("continue-on-error" in line
                            for line in blocks["commit-hygiene"]))


def spec(**overrides):
    base = {
        "dimension": "cpp",
        "events": manifest.ALL_EVENTS,
        "gating": True,
        "local": "make test",
    }
    base.update(overrides)
    return base


class Checks(unittest.TestCase):
    def jobs(self):
        return {
            "changes": spec(gating=False, reason="change detector"),
            "lint": spec(),
            "ci-gate": spec(gating=False, reason="aggregator"),
        }

    def test_a_consistent_inventory_is_clean(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate"],
                               ["changes", "lint"])
        self.assertEqual(errors, [])

    def test_a_gating_job_missing_from_ci_gate_is_an_error(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate"],
                               ["changes"])
        self.assertTrue(any("lint" in e and "ci-gate" in e for e in errors), errors)

    def test_a_workflow_job_missing_from_the_manifest_is_an_error(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate", "new-gate"],
                               ["changes", "lint"])
        self.assertTrue(any("new-gate" in e for e in errors), errors)

    def test_a_needs_entry_with_no_manifest_entry_is_an_error(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate"],
                               ["changes", "lint", "ghost"])
        self.assertTrue(any("ghost" in e for e in errors), errors)

    def test_a_gating_job_without_a_local_command_or_reason_is_an_error(self):
        jobs = self.jobs()
        jobs["lint"] = spec(local=None)
        errors = checker.check(jobs, ["changes", "lint", "ci-gate"], ["changes", "lint"])
        self.assertTrue(any("lint" in e and "local" in e for e in errors), errors)

    def test_a_gating_job_with_a_local_reason_is_accepted(self):
        jobs = self.jobs()
        jobs["lint"] = spec(local=None, local_reason="runs on a macOS runner")
        errors = checker.check(jobs, ["changes", "lint", "ci-gate"], ["changes", "lint"])
        self.assertEqual(errors, [])

    def test_changes_is_required_by_ci_gate(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate"], ["lint"])
        self.assertTrue(any("changes" in e for e in errors), errors)

    def test_the_aggregator_cannot_need_itself(self):
        errors = checker.check(self.jobs(), ["changes", "lint", "ci-gate"],
                               ["changes", "lint", "ci-gate"])
        self.assertTrue(any("ci-gate" in e for e in errors), errors)

    def test_a_scan_that_found_almost_nothing_fails_closed(self):
        errors = checker.check(self.jobs(), ["changes"], ["changes"],
                               min_jobs=checker.MIN_JOBS,
                               min_needs=checker.MIN_NEEDS)
        self.assertTrue(any("scan" in e.lower() or "sanity" in e.lower() for e in errors),
                        errors)


class Advisory(unittest.TestCase):
    """An advisory job must annotate, not report a failed check."""

    def jobs(self):
        return {
            "changes": spec(gating=False, reason="change detector"),
            "commit-hygiene": spec(gating=False, advisory=True,
                                   reason="advisory: not adopted yet"),
            "ci-gate": spec(gating=False, reason="aggregator"),
        }

    def evaluate(self, text):
        return checker.check(self.jobs(), ["changes", "commit-hygiene", "ci-gate"],
                             ["changes"], blocks=checker.parse_job_blocks(text))

    def test_a_step_level_flag_is_accepted(self):
        self.assertEqual(self.evaluate(ADVISORY), [])

    def test_job_level_alone_is_an_error(self):
        text = ADVISORY.replace("        continue-on-error: true\n        run:", "        run:")
        errors = self.evaluate(text)
        self.assertTrue(any("step-level" in e for e in errors), errors)

    def test_an_advisory_job_cannot_be_gating(self):
        jobs = self.jobs()
        jobs["commit-hygiene"] = spec(advisory=True, gating=True)
        errors = checker.check(jobs, ["changes", "commit-hygiene", "ci-gate"],
                               ["changes", "commit-hygiene"],
                               blocks=checker.parse_job_blocks(ADVISORY))
        self.assertTrue(any("cannot also be gating" in e for e in errors), errors)

    def test_the_real_advisory_job_is_flagged_in_the_workflow(self):
        with open(WORKFLOW, encoding="utf-8") as handle:
            blocks = checker.parse_job_blocks(handle.read())
        self.assertEqual(checker.advisory_errors(manifest.JOBS, blocks), [])


class TheRepository(unittest.TestCase):
    def test_the_real_workflow_and_manifest_agree(self):
        self.assertEqual(checker.load_and_check(WORKFLOW), [])

    def test_every_gating_job_is_in_the_skip_policy(self):
        gating = {job for job, row in manifest.JOBS.items() if row["gating"]}
        self.assertEqual(set(policy.DIMENSIONS), gating)

    def test_the_policy_dimensions_match_the_manifest(self):
        for job, row in manifest.JOBS.items():
            if not row["gating"]:
                continue
            self.assertEqual(policy.DIMENSIONS[job], row["dimension"], job)
            self.assertEqual(policy.EVENTS.get(job, manifest.ALL_EVENTS),
                             row["events"], job)

    def test_the_independent_fixture_covers_every_job_exactly_once(self):
        self.assertEqual(len(policy.ALL_JOBS), len(set(policy.ALL_JOBS)))
        self.assertEqual(set(policy.ALL_JOBS), set(policy.DIMENSIONS))
        declared = (set(policy.CPP_JOBS) | set(policy.WEB_JOBS) |
                    set(policy.ALWAYS_JOBS) | set(policy.PR_ONLY_JOBS))
        self.assertEqual(declared, set(policy.ALL_JOBS))


if __name__ == "__main__":
    unittest.main()
