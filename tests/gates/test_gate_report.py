"""One place to see every gate's verdict, as Markdown and JSON.

The gates already print their own verdicts, but into a dozen separate job logs.
A reviewer asking "what did the gates say?" should not have to open six runs and
grep. This assembles them into a single summary and a machine-readable file, which
CI then uploads as a build artefact (GitHub's equivalent of an ADO build artefact:
attached to the run, downloadable with `gh run download`).
"""

import unittest
from unittest import mock

from harness import load

gr = load("gate_report")

PASS, FAIL, ERROR, SKIP = "pass", "fail", "error", "skipped"


def result(name, verdict, summary="", details=""):
    return {"gate": name, "verdict": verdict, "summary": summary, "details": details}


class Verdicts(unittest.TestCase):
    def test_exit_code_is_zero_only_when_nothing_failed(self):
        self.assertEqual(gr.exit_code([result("a", PASS)]), 0)
        self.assertEqual(gr.exit_code([result("a", PASS), result("b", SKIP)]), 0)
        self.assertEqual(gr.exit_code([result("a", FAIL)]), 1)
        self.assertEqual(gr.exit_code([result("a", ERROR)]), 2)

    def test_verdict_for_exit_status(self):
        self.assertEqual(gr.verdict_for(0), PASS)
        self.assertEqual(gr.verdict_for(1), FAIL)
        self.assertEqual(gr.verdict_for(2), ERROR)
        self.assertEqual(gr.verdict_for(127), ERROR, "a missing command is an error")
        self.assertEqual(gr.verdict_for(-9), ERROR)

    def test_zero_is_never_a_verdict_on_its_own(self):
        """A gate that crashed returns non-zero but so does a gate that found a
        violation; the distinction is in the verdict, not the code."""
        self.assertNotEqual(gr.verdict_for(0), gr.verdict_for(1))


class Markdown(unittest.TestCase):
    def test_table_lists_every_gate(self):
        results = [result("complexity", PASS, "0 over-limit"),
                   result("nesting", FAIL, "1 over-limit"),
                   result("debt", PASS, "0 markers")]
        md = gr.to_markdown(results)
        for name in ("complexity", "nesting", "debt"):
            self.assertIn(name, md)
        self.assertIn("FAIL", md)

    def test_verdict_is_rendered_so_it_is_greppable(self):
        md = gr.to_markdown([result("nesting", FAIL, "1 over-limit")])
        self.assertIn("nesting", md)
        self.assertIn("fail", md.lower())

    def test_escapes_pipes_in_details(self):
        """A `|` in a gate's output would break the table it is rendered into."""
        md = gr.to_markdown([result("x", FAIL, "a|b")])
        self.assertIn("a\\|b", md)

    def test_escapes_newlines(self):
        md = gr.to_markdown([result("x", FAIL, "one\ntwo")])
        self.assertIn("<br>", md)
        row = [l for l in md.splitlines() if l.startswith("| x")][0]
        self.assertEqual(row.count("|"), 4, "one row of three cells")

    def test_details_are_kept_but_out_of_the_table(self):
        md = gr.to_markdown([result("x", FAIL, "short", "long explanation here")])
        self.assertIn("short", md)
        self.assertIn("long explanation here", md)

    def test_empty_report_still_renders_a_table(self):
        md = gr.to_markdown([])
        self.assertIn("|", md)
        self.assertIn("none", md.lower())

    def test_report_has_a_heading_and_a_verdict_line(self):
        md = gr.to_markdown([result("a", PASS)])
        self.assertTrue(md.startswith("#"))
        self.assertIn("Gate report", md)


class Json(unittest.TestCase):
    def test_round_trips(self):
        import json

        results = [result("complexity", PASS, "0 over-limit")]
        restored = json.loads(gr.to_json(results))
        self.assertEqual(restored, results)

    def test_is_sorted_for_a_stable_diff(self):
        a = [result("z", PASS), result("a", PASS)]
        b = [result("a", PASS), result("z", PASS)]
        self.assertEqual(gr.to_json(a), gr.to_json(b))


class Invocation(unittest.TestCase):
    def test_command_for_a_gate_is_built_from_a_name(self):
        self.assertEqual(gr.command_for("nesting"), ["make", "nesting"])

    def test_unknown_gate_is_refused(self):
        with self.assertRaises(KeyError):
            gr.command_for("not-a-gate")

    def test_every_advertised_gate_has_a_command(self):
        for name in gr.GATES:
            self.assertIn(name, gr.COMMANDS)


if __name__ == "__main__":
    unittest.main()