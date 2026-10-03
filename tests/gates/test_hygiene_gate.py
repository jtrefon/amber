"""C++ hygiene rules that AGENTS.md states but nothing enforced.

Three checks, all measured clean before being switched on, so all three are
cliffs rather than ratchets:

  using-namespace   `using namespace` in a header leaks every symbol it names
                    into every translation unit that includes it.
  raw-memory        AGENTS.md: "Never use raw new/delete." Found exactly one,
                    and it is legitimate -- Job's constructor is private, so
                    make_unique cannot reach it.
  self-contained    A header that only compiles because some earlier .cpp
                    happened to include something first is a latent build
                    break, and the failure lands on an unrelated file.

The escape hatch is `hygiene-allow: <reason>` on the same line, as with the debt
gate. Suppressions are counted and printed by --report so they cannot accumulate
silently: a gate with no escape gets worked around by deleting the code, which is
worse than an annotated exception.
"""

import io
import os
import unittest
from contextlib import redirect_stdout
from unittest import mock

from harness import load

hg = load("hygiene_gate")

ALLOW = "hygiene-allow:"


class UsingNamespace(unittest.TestCase):
    def test_clean_header(self):
        self.assertEqual(hg.using_namespace_findings("namespace agent {}\n"), [])

    def test_finds_it(self):
        found = hg.using_namespace_findings("using namespace std;\n")
        self.assertEqual(len(found), 1)

    def test_ignores_a_suppressed_line(self):
        self.assertEqual(
            hg.using_namespace_findings(f"using namespace std;  // {ALLOW} why\n"), [])

    def test_qualified_using_declaration_is_not_this_rule(self):
        """`using std::string;` names one symbol and is fine; `using namespace`
        is the one that leaks everything."""
        self.assertEqual(hg.using_namespace_findings("using std::string;\n"), [])

    def test_a_type_alias_is_not_a_using_directive(self):
        self.assertEqual(hg.using_namespace_findings("using Callback = void(*)();\n"), [])


class RawMemory(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(hg.raw_memory_findings("auto p = std::make_unique<T>();\n"), [])

    def test_finds_new(self):
        self.assertEqual(len(hg.raw_memory_findings("auto p = new T;\n")), 1)

    def test_finds_delete(self):
        self.assertEqual(len(hg.raw_memory_findings("delete p;\n")), 1)

    def test_equals_delete_is_not_raw_memory(self):
        """`T& operator=(const T&) = delete;` is required by the Rule of Five and
        appears all over the codebase. It is not a raw delete."""
        self.assertEqual(hg.raw_memory_findings("T& operator=(const T&) = delete;\n"), [])

    def test_a_word_in_a_name_is_not_raw_memory(self):
        for line in ("int newest = 0;", "auto d = deleted_flag;", "// renew the token"):
            self.assertEqual(hg.raw_memory_findings(line + "\n"), [], line)

    def test_acknowledged_use_is_allowed(self):
        self.assertEqual(
            hg.raw_memory_findings(f"auto p = new T;  // {ALLOW} private ctor\n"), [])

    def test_acknowledgement_must_be_on_the_line_itself(self):
        """Matched per line, not per comment block. A marker in a paragraph above
        would otherwise silently suppress the findings of whatever statement
        happens to follow it -- and it did, until this was pinned."""
        source = ("// a long explanation\n"
                  "// spanning several lines\n"
                  f"// {ALLOW} private ctor\n"
                  "auto p = new T;\n")
        self.assertEqual(len(hg.raw_memory_findings(source)), 1)

    def test_explanation_above_plus_marker_on_the_line_is_allowed(self):
        source = ("// Job's constructor is private, so make_unique cannot reach it.\n"
                  "auto p = new T;  // hygiene-allow: private ctor\n")
        self.assertEqual(hg.raw_memory_findings(source), [])


class SuppressionCounting(unittest.TestCase):
    """A gate with no escape hatch gets worked around by deleting the code."""

    def test_counts_suppressions(self):
        source = f"using namespace std;  // {ALLOW} legacy\nint x;\n"
        self.assertEqual(hg.count_suppressions(source), 1)

    def test_zero_when_none(self):
        self.assertEqual(hg.count_suppressions("int x;\n"), 0)


class Scope(unittest.TestCase):
    def test_benchmark_fixtures_are_excluded(self):
        """bench/scenarios/ holds code the benchmark *agent* writes and an oracle
        scores. It is deliberately not project style — the same reason cppcheck and
        the format gate skip it."""
        self.assertTrue(any(p.startswith("bench/scenarios")
                            for p in hg.EXCLUDED_PREFIXES))

    def test_excluded_paths_are_skipped(self):
        for path in ("bench/scenarios/coding/sorting/reference/sorting.cpp",
                     "third_party/nlohmann/json.hpp", "include/nlohmann/json.hpp"):
            self.assertTrue(hg.is_excluded(path), path)

    def test_project_paths_are_not_excluded(self):
        for path in ("lib/job.cpp", "tui/tui.h", "tools/read_tool.cpp"):
            self.assertFalse(hg.is_excluded(path), path)


class GateContract(unittest.TestCase):
    def test_clean_tree_passes(self):
        buf = io.StringIO()
        with mock.patch.object(hg, "scan", return_value=({}, {}, 100)):
            with redirect_stdout(buf):
                rc = hg.check({}, {}, 100)
        self.assertEqual(rc, 0, buf.getvalue())
        self.assertIn("clean", buf.getvalue())

    def test_any_finding_fails(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = hg.check({"lib/a.cpp": [(3, "using namespace std;")]}, {}, 100)
        self.assertEqual(rc, 1)
        self.assertIn("lib/a.cpp:3", buf.getvalue())
        self.assertIn(ALLOW, buf.getvalue())

    def test_empty_scan_fails_closed(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = hg.check({}, {}, 0)
        self.assertEqual(rc, 2)
        self.assertIn("scanned", buf.getvalue().lower())


if __name__ == "__main__":
    unittest.main()