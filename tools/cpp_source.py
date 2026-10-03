#!/usr/bin/env python3
"""A small, shared C++ lexer for the source-scanning gates.

Both `class_size_gate.py` (which must find where a type's braces balance) and
`nesting_gate.py` (which must find where control flow nests) need the same
thing: the source with everything that is *data* rather than *structure* removed,
while keeping every character position intact.

The distinction matters. A `}` inside a string literal, a character literal, a
comment or a raw string is punctuation in data, not a block terminator. Counting
it is how a large class measures as small — `bench/probe.cpp` carries SSE
payloads whose escaped JSON has unbalanced braces inside string literals.

This is deliberately a lexer, not a parser. It does not understand
preprocessor conditionals, macros that expand to braces, or template syntax; it
understands comments and literals, which is what the gates actually need. Where it
cannot be sure, the callers fail closed rather than guessing (see
`unbalanced_braces`).
"""

# A C++ raw string delimiter is at most 16 characters, so a `(` further away than
# that cannot be the body of a raw string.
MAX_RAW_DELIM = 16

BLOCK_OPENERS = ("{",)
BLOCK_CLOSERS = ("}",)


def keep_newlines(span):
    """`span` with every non-newline character blanked, so line numbering and
    column positions survive while the content cannot be mistaken for code."""
    return "".join("\n" if c == "\n" else " " for c in span)


def skip_literal(text, start):
    """Index just past the string or character literal beginning at `start`,
    honouring backslash escapes.

    Prefixed literals (`L"..."`, `u8"..."`) need no special handling: their body
    is scanned the same way. Raw strings (`R"delim(...)delim"`) are handled by
    `skip_raw_string`, which the caller must reach first — see `strip_noise`.
    """
    quote = text[start]
    i = start + 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        if text[i] == "\n":
            return i  # an unterminated literal ends at the line break
        i += 1
    return i


def skip_raw_string(text, start):
    """Index just past a raw string literal beginning at `start` (`R"delim( )`)."""
    open_paren = text.find("(", start)
    if open_paren < 0 or open_paren - start > MAX_RAW_DELIM:
        return len(text)
    closer = ")" + text[start + 2:open_paren] + '"'
    end = text.find(closer, open_paren)
    return len(text) if end < 0 else end + len(closer)


def strip_noise(text):
    """Comments and literals blanked out, every other character preserved.

    Line structure is preserved exactly — every newline in the input produces one
    newline in the output — so the result can be split into lines and indexed
    alongside the original.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            end = text.find("\n", i)
            if end < 0:
                break
            i = end
            continue
        if two == "/*":
            start = i
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(keep_newlines(text[start:i]))
            continue
        char = text[i]
        # The raw-string prefix must be recognised here, not inside
        # skip_literal: the loop reaches `R` as ordinary code and then meets the
        # quote, which would scan the body as a normal literal and stop at the
        # first embedded quote.
        if two in ('R"', "R'"):
            start = i
            i = skip_raw_string(text, i)
            out.append(keep_newlines(text[start:i]))
            continue
        if char == '"' or char == "'":
            start = i
            i = skip_literal(text, i)
            out.append(keep_newlines(text[start:i]))
            continue
        out.append(char)
        i += 1
    return "".join(out)


def read(path):
    """(source, structural) for a file, or (None, None) if it cannot be read.

    `structural` is `source` with comments and literals blanked.
    """
    try:
        with open(path, encoding="utf-8", errors="ignore") as handle:
            source = handle.read()
    except OSError:
        return None, None
    return source, strip_noise(source)


def structural_lines(path):
    try:
        return strip_noise(read(path)[0]).split("\n")
    except TypeError:
        return None


def unbalanced_braces(lines):
    """True when the structural text leaves a brace open at end of input.

    A preprocessor conditional can legitimately unbalance it (`#ifdef A { ... }`
    with the `}` compiled out), so callers treat this as "cannot be measured with
    confidence", not as a syntax error.
    """
    depth = 0
    for line in lines:
        depth += line.count("{") - line.count("}")
        if depth < 0:
            return True
    return depth != 0
