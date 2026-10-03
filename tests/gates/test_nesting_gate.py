"""Nesting depth of control flow, measured by our own lexer.

lizard cannot do this: its `-w` output ends with an `ND` field
(`max_nesting_depth`), and for C++ that field is 0 for every function — verified
against a deliberately 6-deep function. Gating on it would be a gate that is
always green because it measures nothing, which is the exact failure mode the
other gates were just fixed to reject.

So nesting is measured here, from the source, with the shared lexer. The
definition matches the common industry ones (SonarQube's nesting depth, ESLint's
max-depth): the maximum number of *nested control structures*, where a control
structure's own body counts as one level and the enclosing function does not.
"""

import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

from harness import load

ng = load("nesting_gate")


def depth_of(body, function="void f()"):
    """Max nesting inside `function`, given its full source text."""
    source = function + " {\n" + body + "\n}\n"
    lines = ng.strip_noise(source).split("\n")
    return ng.max_nesting(lines, start=0)


class SimpleNesting(unittest.TestCase):
    def test_flat_function_is_zero(self):
        self.assertEqual(depth_of("    int a = 1; a++; return;"), 0)

    def test_single_if_is_one(self):
        self.assertEqual(depth_of("    if (a) { b(); }"), 1)

    def test_if_without_braces_is_zero(self):
        self.assertEqual(depth_of("    if (a) b();"), 0)

    def test_if_then_nested_if_is_two(self):
        self.assertEqual(depth_of("    if (a) { if (b) { c(); } }"), 2)

    def test_three_levels(self):
        self.assertEqual(depth_of("    if (a) { for (;;) { while (b) { c(); } } }"), 3)

    def test_else_does_not_add_a_level(self):
        self.assertEqual(depth_of("    if (a) { b(); } else { c(); }"), 1)

    def test_else_if_is_one_level_not_two(self):
        self.assertEqual(depth_of("    if (a) { b(); } else if (c) { d(); }"), 1)

    def test_sibling_blocks_do_not_accumulate(self):
        self.assertEqual(
            depth_of("    if (a) { if (b) { x(); } }\n    if (c) { if (d) { y(); } }"), 2)

    def test_switch_counts_as_one(self):
        self.assertEqual(depth_of("    switch (a) { case 1: b(); break; }"), 1)

    def test_try_catch_counts_as_one(self):
        self.assertEqual(depth_of("    try { a(); } catch (...) { b(); }"), 1)

    def test_do_while_counts_as_one(self):
        self.assertEqual(depth_of("    do { a(); } while (b);"), 1)

    def test_range_for_counts(self):
        self.assertEqual(depth_of("    for (auto x : xs) { use(x); }"), 1)


class WhatIsNotNesting(unittest.TestCase):
    """Only control structures nest. Everything else must not inflate the count,
    or the number stops meaning anything."""

    def test_plain_braces_do_not_nest(self):
        self.assertEqual(depth_of("    struct S { int a; }; S s;"), 0)

    def test_initializer_list_does_not_nest(self):
        self.assertEqual(depth_of("    std::vector<int> v{1, 2, 3};"), 0)

    def test_lambda_body_is_not_a_level(self):
        self.assertEqual(depth_of("    auto f = []() { return 1; }; f();"), 0)

    def test_nested_lambda_still_not_a_level(self):
        self.assertEqual(depth_of("    auto f = []() { auto g = []() { return 1; }; }"), 0)

    def test_braced_init_in_a_call(self):
        self.assertEqual(depth_of("    f({1, 2});"), 0)

    def test_ternary_does_not_nest(self):
        self.assertEqual(depth_of("    int x = a ? b : c;"), 0)

    def test_a_word_containing_a_keyword(self):
        self.assertEqual(depth_of("    int iflag = 1; int forx = 2;"), 0)

    def test_endif_is_not_if(self):
        self.assertEqual(depth_of("    int a = 1;"), 0)

    def test_member_access_is_not_a_keyword(self):
        self.assertEqual(depth_of("    obj.for_each(1);"), 0)

    def test_string_containing_a_keyword_and_brace(self):
        self.assertEqual(depth_of('    const char* s = "if (a) { }";'), 0)

    def test_comment_containing_a_keyword_and_brace(self):
        self.assertEqual(depth_of("    // if (a) { }"), 0)


class InteractionWithRealNesting(unittest.TestCase):
    def test_nested_if_inside_a_lambda_inside_an_if(self):
        body = "    if (a) {\n        auto f = []() { if (b) { c(); } };\n        f();\n    }"
        self.assertEqual(depth_of(body), 2)

    def test_deep_chain_is_measured_exactly(self):
        """if > for > while > do > switch > if = 6 levels. Counting the enclosing
        function's own brace would report 7."""
        body = "    if (a) { for (;;) { while (b) { do { switch (c) { "
        body += "case 1: if (d) { e(); } } } } } }"
        self.assertEqual(depth_of(body), 6)


class FunctionExtent(unittest.TestCase):
    """lizard's reported `length` undercounts a C++ function-try-block, so the
    extent is derived here instead."""

    def lines(self, source):
        return ng.strip_noise(source).split("\n")

    def test_plain_function(self):
        src = "int f() {\n    a();\n}\nint g() {\n    b();\n}\n"
        lines = self.lines(src)
        self.assertEqual(ng.function_extent(lines, 0), (3, True))

    def test_function_try_block_is_one_function(self):
        """`int main() try { ... } catch (...) { ... }` closes the try body
        mid-way, so a naive depth check would stop there and treat the handler as a
        separate function. Every main in this repo is written this way."""
        src = ("int main() try {\n    a();\n} catch (const E& e) {\n"
               "    b();\n}\nint next() {\n    c();\n}\n")
        end, balanced = ng.function_extent(self.lines(src), 0)
        self.assertTrue(balanced)
        self.assertEqual(end, 5, "must span the whole function including the catch")

    def test_one_line_body(self):
        """`Foo(...): x_(y) {}` opens and closes without the depth ever rising."""
        src = "class C {\n    C(int a) : x_(a) {}\n    int f() { return 1; }\n};\n"
        end, balanced = ng.function_extent(self.lines(src), 1)
        self.assertTrue(balanced)
        self.assertEqual(end, 2)

    def test_unterminated_function(self):
        src = "int f() {\n    a();\n"
        end, balanced = ng.function_extent(self.lines(src), 0)
        self.assertFalse(balanced)


class CatchIsNotAFunction(unittest.TestCase):
    """lizard reports a function-try-block's handler as a function named `catch`."""

    def test_catch_lines_are_dropped(self):
        line = ("src/main.cpp:648: warning: catch has 4 NLOC, 1 CCN, 20 token, "
                "1 PARAM, 4 length, 0 ND")
        functions, unparsed = ng.parse_lizard_output([line])
        self.assertEqual(functions, [])
        self.assertEqual(unparsed, 0)

    def test_real_functions_are_kept(self):
        line = ("lib/a.cpp:12: warning: agent::f has 5 NLOC, 2 CCN, 30 token, "
                "1 PARAM, 9 length, 0 ND")
        functions, _ = ng.parse_lizard_output([line])
        self.assertEqual(len(functions), 1)
        self.assertEqual(functions[0]["name"], "agent::f")


class NestingByFileHandlesRealSource(unittest.TestCase):
    def test_a_file_that_never_balances_is_still_measured(self):
        """`tui/welcome.cpp` opens an array initialiser and closes it in an
        `#include`d .inc, so the file never balances. A file-level check would
        reject it; every function in it is perfectly measurable."""
        import os
        import tempfile

        source = ('#include "art_data.inc"\n'
                  'const unsigned char kArt[2][2] = {\n'
                  'void render() {\n'
                  '    if (a) {\n'
                  '        for (;;) {\n'
                  '            b();\n'
                  '        }\n'
                  '    }\n'
                  '}\n')
        handle = tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False)
        handle.write(source)
        handle.close()
        try:
            entries = [{"file": handle.name, "line": 4, "name": "render", "length": 6}]
            depths, unmeasurable = ng.nesting_by_file(entries)
            self.assertEqual(unmeasurable, {})
            self.assertEqual(depths[(handle.name, "render")], 2)
        finally:
            os.unlink(handle.name)


class GateContract(unittest.TestCase):
    """Every case gets its own temp baseline, so the contract under test does not
    depend on what the repository's baseline currently holds."""

    def setUp(self):
        self.baseline = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"nesting_max": 4, "depths": {}}, self.baseline)
        self.baseline.close()
        patch = mock.patch.object(ng, "BASELINE", self.baseline.name)
        patch.start()
        self.addCleanup(patch.stop)
        self.addCleanup(os.unlink, self.baseline.name)

    def over(self, depth):
        return {("lib/a.cpp", "f"): depth}

    def run_check(self, over, measured=None, unmeasurable=None):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ng.check(over, ng.MIN_FUNCTIONS_SANE if measured is None else measured,
                          unmeasurable or {})
        return rc, buf.getvalue()

    def test_cap_is_declared(self):
        self.assertGreater(ng.NESTING_MAX, 0)

    def test_clean_tree_passes(self):
        rc, out = self.run_check({})
        self.assertEqual(rc, 0, out)
        self.assertIn("baseline holds", out)

    def test_new_deep_function_fails(self):
        rc, out = self.run_check(self.over(ng.NESTING_MAX + 1))
        self.assertEqual(rc, 1)
        self.assertIn("f", out)

    def test_empty_scan_fails_closed(self):
        rc, out = self.run_check({}, measured=0)
        self.assertEqual(rc, 2)
        self.assertIn("measured", out.lower())

    def test_implausibly_small_scan_fails_closed(self):
        rc, _ = self.run_check({}, measured=3)
        self.assertEqual(rc, 2)

    def test_unmeasurable_source_fails_closed(self):
        rc, out = self.run_check({}, unmeasurable={("lib/a.cpp", "f"): "no closing brace"})
        self.assertEqual(rc, 2)
        self.assertIn("lib/a.cpp", out)

    def test_unmeasurable_beats_a_real_violation(self):
        rc, _ = self.run_check(self.over(9),
                               unmeasurable={("lib/a.cpp", "g"): "no closing brace"})
        self.assertEqual(rc, 2,
                         "an unmeasurable function must not be reported as a violation")

    def test_liveness_signals_cannot_be_omitted(self):
        with self.assertRaises(TypeError):
            ng.check({})

    def test_baseline_only_shrinks(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            ng.update(self.over(ng.NESTING_MAX + 3))
        self.assertEqual(ng.load_baseline(), {("lib/a.cpp", "f"): ng.NESTING_MAX + 3})
        with redirect_stdout(io.StringIO()):
            self.assertEqual(ng.check(self.over(ng.NESTING_MAX + 4), 5000, {}), 1,
                             "a function that got deeper must fail")
            self.assertEqual(ng.check(self.over(ng.NESTING_MAX + 1), 5000, {}), 0,
                             "a function that got shallower must pass")
            self.assertEqual(ng.check(self.over(ng.NESTING_MAX + 3), 5000, {}), 0)

    def test_update_writes_the_cap_it_used(self):
        with redirect_stdout(io.StringIO()):
            ng.update(self.over(5))
        with open(self.baseline.name, encoding="utf-8") as handle:
            self.assertEqual(json.load(handle)["nesting_max"], ng.NESTING_MAX)

    def test_absent_baseline_is_an_error(self):
        with mock.patch.object(ng, "BASELINE", "/nonexistent/nesting_baseline.json"):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(ng.check({}, 5000, {}), 2)


if __name__ == "__main__":
    unittest.main()
