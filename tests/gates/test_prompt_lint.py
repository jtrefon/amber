"""The prompting philosophy is descriptive, and the gate has to tell that apart
from a prohibition.

"what you do not know, you mark as such" describes a behaviour. "Do not guess."
forbids one. A gate that flagged every occurrence of "do not" would fail the
first sentence and get switched off; the sentence-initial rule is what makes the
check useful.
"""

import unittest

from harness import REPO_ROOT, load

prompt = load("prompt_lint")


class Findings(unittest.TestCase):
    def test_a_sentence_initial_never_is_flagged(self):
        self.assertEqual(len(prompt.findings("Never touch the service.\n")), 1)

    def test_a_sentence_initial_do_not_is_flagged(self):
        self.assertEqual(len(prompt.findings("Do not guess.\n")), 1)

    def test_a_sentence_initial_must_is_flagged(self):
        self.assertEqual(len(prompt.findings("You must retry.\n")), 0, "descriptive")
        self.assertEqual(len(prompt.findings("Must retry.\n")), 1)

    def test_a_markdown_list_item_is_flagged(self):
        self.assertEqual(len(prompt.findings("- Never skip the check.\n")), 1)

    def test_a_heading_is_flagged(self):
        self.assertEqual(len(prompt.findings("## Must\n")), 1)

    def test_mid_sentence_do_not_is_descriptive(self):
        line = "on facts you have checked: what you know, you state plainly; what you do not\n"
        self.assertEqual(prompt.findings(line), [])

    def test_a_fenced_code_block_is_ignored(self):
        text = "```sh\n# Do not run this\n```\n"
        self.assertEqual(prompt.findings(text), [])

    def test_a_clean_prompt_has_no_findings(self):
        self.assertEqual(prompt.findings("You are a careful engineer.\n"), [])


class Scope(unittest.TestCase):
    def test_the_coding_library_is_reference_not_instruction(self):
        self.assertFalse(prompt.is_scoped(
            "prompts/domains/coding/library/solid-violations-by-symptom.md"))

    def test_agent_prompts_are_scoped(self):
        for path in ("prompts/system.md", "prompts/tools.md",
                     "prompts/tools/read.md", "prompts/watch/system.md"):
            self.assertTrue(prompt.is_scoped(path), path)

    def test_non_prompts_are_not_scoped(self):
        for path in ("AGENTS.md", "docs/architecture.md", "prompts/data.json"):
            self.assertFalse(prompt.is_scoped(path), path)


class TheTree(unittest.TestCase):
    def test_the_scoped_prompts_are_clean(self):
        found, scanned = prompt.scan(REPO_ROOT)
        self.assertGreaterEqual(scanned, prompt.MIN_FILES_SANE)
        self.assertEqual(found, {})


if __name__ == "__main__":
    unittest.main()
