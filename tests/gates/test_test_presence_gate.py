"""A production change without a test change is the failure the red-first rule
is trying to prevent. The gate cannot see commit order, but it can see whether
the suite was touched at all -- and a waiver keeps a legitimate exception from
turning into a bypass.
"""

import unittest

from harness import load

presence = load("test_presence_gate")


class ProductionPaths(unittest.TestCase):
    def test_compiled_project_sources_are_production(self):
        files = ("lib/agent.cpp", "tui/tui.h", "src/main.cpp",
                 "tools/read_tool.cpp", "plugins/clock/clock_plugin.cpp",
                 "bench/kpi.cpp")
        self.assertEqual(presence.production_paths(files), list(files))

    def test_non_code_is_not_production(self):
        files = ("README.md", "docs/architecture.md", "prompts/system.md",
                 "Makefile.in", ".github/workflows/ci.yml",
                 "website/index.html", "completions.json")
        self.assertEqual(presence.production_paths(files), [])

    def test_benchmark_fixtures_are_not_production(self):
        self.assertEqual(presence.production_paths(
            ["bench/scenarios/coding/sorting/reference/sorting.cpp"]), [])


class Assessment(unittest.TestCase):
    def test_production_without_tests_fails(self):
        bad = presence.assess(["lib/agent.cpp", "docs/a.md"])
        self.assertEqual(bad, ["lib/agent.cpp"])

    def test_a_test_change_satisfies_the_rule(self):
        self.assertEqual(presence.assess(["lib/agent.cpp", "tests/agent_test.cpp"]), [])

    def test_docs_only_is_clean(self):
        self.assertEqual(presence.assess(["docs/a.md", "README.md"]), [])

    def test_a_waiver_in_the_body_is_accepted(self):
        self.assertEqual(
            presence.assess(["lib/agent.cpp"],
                            body="No test: tests-allow: covered by the pty harness"), [])

    def test_a_waiver_in_a_commit_message_is_accepted(self):
        self.assertEqual(
            presence.assess(["lib/agent.cpp"], commits="fix: x\n\ntests-allow: no seam"),
            [])

    def test_a_waiver_without_a_reason_is_still_a_waiver(self):
        """The marker is the contract; requiring prose in the parser would make a
        legitimate exception fail for a formatting reason."""
        self.assertEqual(presence.assess(["lib/agent.cpp"], body="tests-allow:"), [])


if __name__ == "__main__":
    unittest.main()
