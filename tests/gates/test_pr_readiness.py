"""The PR readiness check.

The repo has one collaborator and GitHub forbids self-approval, so the
mechanical half of "a PR is a request to review finished work" is enforced here:
no drafts, verification evidence in the body, and `review:approved` on the PR
categories that need it. The "one completed feature" rule stays a review
judgement.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from harness import load

ready = load("pr_readiness")


def event(body="", draft=False, labels=None, kind="pull_request", user=""):
    payload = {"pull_request": {"body": body, "draft": draft, "labels": labels or [],
                                "user": {"login": user}}}
    if kind:
        payload["action"] = "opened"
        payload["pull_request"]["title"] = "a feature"
    return payload


class Readiness(unittest.TestCase):
    EVIDENCE = ("## Verification\n\n"
                "- `make verify-quick` — clean\n"
                "- manual run: the button now closes the dialog\n")

    def rc(self, payload):
        return ready.check(payload)[0]

    def test_a_complete_pr_is_ready(self):
        self.assertEqual(self.rc(event(self.EVIDENCE)), 0)

    def test_a_draft_is_not_ready(self):
        self.assertEqual(self.rc(event(self.EVIDENCE, draft=True)), 1)

    def test_a_body_without_verification_is_not_ready(self):
        self.assertEqual(self.rc(event("## Summary\n\nA change.")), 1)

    def test_the_template_placeholders_are_not_evidence(self):
        body = ("## Manual end-to-end verification\n\n"
                "- **Steps:**\n  1.\n- **Environment:**\n- **Expected:**\n"
                "- **Observed:**\n- **Evidence:**\n")
        self.assertEqual(self.rc(event(body)), 1)

    def test_a_populated_manual_e2e_section_is_evidence(self):
        body = ("## Manual end-to-end verification\n\n"
                "- **Steps:** open the dialog, press Esc\n"
                "- **Environment:** Ubuntu 24.04, amber 0.9, build abc123\n"
                "- **Expected:** the dialog closes\n"
                "- **Observed:** the dialog closed\n")
        self.assertEqual(self.rc(event(body)), 0)

    def test_a_tests_and_verification_heading_counts(self):
        self.assertEqual(self.rc(event("## Tests and verification\n\n- 530 tests OK\n")), 0)

    def test_a_checklist_only_verification_section_is_not_evidence(self):
        body = "## Verification\n\n- [ ] make verify-quick\n- [x] CI green\n"
        self.assertEqual(self.rc(event(body)), 1)

    def test_a_review_category_needs_the_approved_label(self):
        for name in ("feature", "bug", "security", "ci"):
            with self.subTest(label=name):
                self.assertEqual(self.rc(event(self.EVIDENCE, labels=[{"name": name}])), 1)

    def test_approved_satisfies_the_review_category(self):
        labels = [{"name": "feature"}, {"name": "review:approved"}]
        self.assertEqual(self.rc(event(self.EVIDENCE, labels=labels)), 0)

    def test_unlabelled_and_docs_prs_are_gate_only(self):
        self.assertEqual(self.rc(event(self.EVIDENCE)), 0)
        self.assertEqual(self.rc(event(self.EVIDENCE, labels=[{"name": "docs"}])), 0)

    def test_dependabot_prs_are_gate_only_without_a_body(self):
        self.assertEqual(self.rc(event("", user="dependabot[bot]")), 0)
        self.assertEqual(self.rc(event("", labels=[{"name": "dependencies"}])), 0)

    def test_a_non_pr_event_is_out_of_scope(self):
        self.assertEqual(self.rc({"push": {"ref": "refs/heads/main"}}), 0)

    def test_a_missing_event_file_fails_closed(self):
        argv = ["pr_readiness.py", "--event", "/nonexistent/event.json"]
        import sys
        old = sys.argv
        sys.argv = argv
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                rc = ready.main()
        finally:
            sys.argv = old
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
