"""The shared C++ lexer: comments and literals are data, not structure.

A `}` inside a string, a comment or a raw string must not close a block, and
blanking them must not move a single line. Both gates that scan source
(`class_size_gate`, `nesting_gate`) depend on this, so it is tested directly
rather than only through them.
"""

import unittest

from harness import load

cs = load("cpp_source")


def blanked(source):
    return cs.strip_noise(source).split("\n")


class PreservesLineStructure(unittest.TestCase):
    """Structural text is indexed alongside source text, so the two must always
    have the same number of lines."""

    def assert_same_shape(self, source):
        self.assertEqual(len(blanked(source)), len(source.split("\n")))

    def test_plain_source(self):
        self.assert_same_shape("int a;\nint b;\n")

    def test_line_comment(self):
        self.assert_same_shape("int a;\n// note\nint b;\n")

    def test_block_comment(self):
        self.assert_same_shape("int a;\n/* one\ntwo\nthree */\nint b;\n")

    def test_string_with_escaped_quote(self):
        self.assert_same_shape('const char* s = "a\\"b";\nint c;\n')

    def test_raw_string_across_lines(self):
        self.assert_same_shape('auto s = R"json({\n"a": 1\n})json";\nint d;\n')

    def test_unterminated_block_comment(self):
        self.assert_same_shape("int a;\n/* never closed\nint b;\n")

    def test_char_literal(self):
        self.assert_same_shape("char c = '}';\nint e;\n")

    def test_escaped_backslash_in_string(self):
        self.assert_same_shape('const char* s = "a\\\\";\nint f;\n')


class BlanksBracesInData(unittest.TestCase):
    def count_braces(self, source):
        text = "\n".join(blanked(source))
        return text.count("{") - text.count("}")

    def test_brace_in_string_is_blanked(self):
        self.assertEqual(self.count_braces('const char* k = "}";'), 0)

    def test_opening_brace_in_string_is_blanked(self):
        self.assertEqual(self.count_braces('const char* k = "{";'), 0)

    def test_brace_in_line_comment_is_blanked(self):
        self.assertEqual(self.count_braces("int a; // {\n"), 0)

    def test_brace_in_block_comment_is_blanked(self):
        self.assertEqual(self.count_braces("int a; /* } */\n"), 0)

    def test_brace_in_raw_string_is_blanked(self):
        self.assertEqual(self.count_braces('auto s = R"x(})x";'), 0)

    def test_real_braces_survive(self):
        self.assertEqual(self.count_braces("void f() { int a; }"), 0)
        self.assertEqual(cs.strip_noise("void f() {").count("{"), 1)


class RawStrings(unittest.TestCase):
    def test_empty_delimiter(self):
        self.assertEqual(cs.strip_noise('auto s = R"({)";').count("{"), 0)

    def test_custom_delimiter(self):
        self.assertEqual(cs.strip_noise('auto s = R"json({)json";').count("{"), 0)

    def test_embedded_quote_and_backslash(self):
        source = 'auto s = R"x(a"b\\c)x";\nint after;'
        self.assertIn("int after;", cs.strip_noise(source))

    def test_unterminated_raw_string_consumes_the_rest(self):
        self.assertNotIn("int", cs.strip_noise('auto s = R"x(oops\nint a;'))


class UnbalancedBraceDetection(unittest.TestCase):
    """The signal a gate uses to refuse to guess."""

    def test_balanced_source(self):
        self.assertFalse(cs.unbalanced_braces(blanked("void f() { if (a) { } }")))

    def test_unclosed_brace(self):
        self.assertTrue(cs.unbalanced_braces(blanked("void f() { if (a) {")))

    def test_extra_closing_brace(self):
        self.assertTrue(cs.unbalanced_braces(blanked("void f() { } }")))

    def test_preprocessor_conditional_is_reported_as_unbalanced(self):
        """`#ifdef A { ... }` can leave the count off; callers must treat that as
        'cannot measure', not as a syntax error."""
        self.assertTrue(cs.unbalanced_braces(blanked("#ifdef A\nvoid f() {\n#endif\n")))


class Read(unittest.TestCase):
    def test_missing_file(self):
        self.assertEqual(cs.read("/nonexistent/file.cpp"), (None, None))

    def test_reads_source_and_structural(self):
        import os
        import tempfile

        handle = tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False)
        handle.write('void f() { const char* k = "}"; }\n')
        handle.close()
        try:
            source, structural = cs.read(handle.name)
            self.assertIn('"}"', source)
            # the literal's brace is gone; the function's own braces remain
            self.assertNotIn('"}"', structural)
            self.assertEqual(structural.count("{"), structural.count("}"))
        finally:
            os.unlink(handle.name)


if __name__ == "__main__":
    unittest.main()
