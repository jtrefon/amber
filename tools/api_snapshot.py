#!/usr/bin/env python3
"""Source-compatibility gate for the plugin API.

`docs/spec/plugins/api-compatibility.md` promises that the documented plugin
surface is source-stable within 1.x: a removal or a signature change needs a
deprecation release, while additions are allowed. Nothing checked that promise:
every consumer is in-tree, so deleting a public method still compiled the tree.

This tool records the public declarations of the promised headers in
`tests/api_snapshot.txt`. The gate requires every recorded declaration to still
exist. A missing entry is a removal, a rename or a signature change; a new entry
is additive and passes, with a note that the snapshot can be refreshed. The
comparison is deliberately a subset check, because "additive changes are
allowed" is the policy.

Only declarations a plugin author can use are recorded:

- namespaces, and the public part of classes/structs (a class starts private, a
  struct starts public; `public:`/`private:`/`protected:` are tracked);
- methods, constructors, destructors, operators, fields, aliases;
- enum names and their enumerators (a new enumerator is additive);
- object-like and function-like macros in the headers.

Function bodies are skipped: an inline `{ return {}; }` is implementation, not
surface. Comments and literals are blanked first (`tools/cpp_source.py`), so a
change to a comment is not an API change.

The snapshot is generated, reviewed and committed:

    python3 tools/api_snapshot.py --update

`make api-check` runs the comparison and compiles
`tests/api/public_consumer.cpp`, an external-style translation unit that
includes only the promised headers and uses the stable types and registries.
The compiler is the second oracle: it catches a broken signature that a text
extractor might mis-read.

Usage:
  tools/api_snapshot.py --check
  tools/api_snapshot.py --update
  tools/api_snapshot.py --check --headers a.h b.h --snapshot out.txt
"""

import argparse
import os
import re
import sys

from cpp_source import strip_noise

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The headers the policy names. `plugin_capability.h` is included separately
# because plugin.h only forward-declares its types; a plugin author includes it.
HEADERS = (
    "include/agent/plugin.h",
    "include/agent/plugin_core.h",
    "include/agent/plugin_capability.h",
    "include/agent/extensions.h",
    "include/agent/events.h",
)

SNAPSHOT = os.path.join(REPO_ROOT, "tests", "api_snapshot.txt")

# A parser that silently returns nothing would make the gate green on an empty
# snapshot. The real surface is several hundred entries, so anything below this
# floor is a measurement failure, not a quiet tree.
MIN_ENTRIES = 100

# `class IPlugin`, `struct EventTraits<TurnStartedEvent>`, `union U`. Anchored at
# the start so `enum class CapabilityKind` is not mistaken for a class. The
# template argument is kept because it distinguishes specialisations.
TYPE_RE = re.compile(
    r"^(?:template\s*<[^>]*>\s*)?(class|struct|union)\s+([A-Za-z_]\w*(?:\s*<[^<>{};]*>)?)")
ENUM_RE = re.compile(r"^enum(?:\s+class)?\s+([A-Za-z_]\w*)")
NAMESPACE_RE = re.compile(r"^namespace\s+([A-Za-z_]\w*)?")
ACCESS = ("public", "private", "protected")


def normalise(text):
    return " ".join(text.split())


class Scope:
    def __init__(self, kind, name="", access="public"):
        self.kind = kind  # namespace | type | enum | body | linkage
        self.name = name
        self.access = access


def in_public_scope(scopes):
    for scope in scopes:
        if scope.kind in ("body", "linkage"):
            return False
        if scope.kind in ("type", "enum") and scope.access != "public":
            return False
    return True


def scope_name(scopes):
    return "::".join(s.name for s in scopes if s.name)


def class_name(header):
    match = TYPE_RE.search(header)
    return normalise(match.group(2)) if match else ""


def inject_macros(text, entries):
    """Object/function-like macros, including `\\` continuations."""
    lines = text.split("\n")
    index = 0
    while index < len(lines):
        line = lines[index].strip()
        index += 1
        if not line.startswith("#define"):
            continue
        while line.endswith("\\") and index < len(lines):
            line = line[:-1] + " " + lines[index].strip()
            index += 1
        entries.add(normalise(line))


def extract_header(text):
    """The public declarations of one header, as normalised snapshot lines."""
    entries = set()
    inject_macros(text, entries)
    clean = strip_noise(text)
    scopes = []
    chunk = []
    paren = bracket = initializer = 0
    i, n = 0, len(clean)

    def record(declaration, terminator=";"):
        declaration = normalise(declaration)
        if not declaration or not in_public_scope(scopes):
            return
        lowered = declaration
        if lowered.startswith(("using namespace ", "friend ", "static_assert",
                               "typedef struct ", "extern template ")):
            return
        where = scope_name(scopes)
        if terminator == ";" and re.match(
                r"^(class|struct|union|enum(?:\s+class)?)\s+[A-Za-z_]\w*\s*$", declaration):
            # A forward declaration and a later definition describe the same
            # type; normalising both to the definition line dedupes the set.
            entries.add((where + " | " if where else "") + declaration)
            return
        if terminator == "{":
            entries.add(where + " | " + declaration if where else declaration)
        elif terminator == ",":
            entries.add((where + "::" if where else "") + declaration)
        else:
            entries.add((where + " | " if where else "") + declaration + ";")

    while i < n:
        char = clean[i]
        if char == "#" and not "".join(chunk).strip():
            end = clean.find("\n", i)
            i = n if end < 0 else end + 1
            chunk = []
            continue
        if char == "(":
            paren += 1
        elif char == ")":
            paren = max(0, paren - 1)
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket = max(0, bracket - 1)
        if char == "{" and paren == 0 and bracket == 0 and initializer == 0:
            header = normalise("".join(chunk))
            chunk = []
            if header.startswith("namespace"):
                match = NAMESPACE_RE.search(header)
                scopes.append(Scope("namespace", match.group(1) if match and match.group(1) else ""))
            elif header.startswith(("extern", "inline namespace")):
                scopes.append(Scope("linkage"))
            elif ENUM_RE.search(header):
                record(header, "{")
                scopes.append(Scope("enum", ENUM_RE.search(header).group(1)))
            elif TYPE_RE.search(header) and not header.startswith(("using ", "typedef ")):
                match = TYPE_RE.search(header)
                kind, name = match.group(1), normalise(match.group(2))
                record(header, "{")
                access = "public" if kind in ("struct", "union") else "private"
                scopes.append(Scope("type", name, access))
            elif "(" in header and not header.startswith(("=", "return", "case")):
                record(header, "{")
                scopes.append(Scope("body"))
            else:
                # A braced initialiser or a control block: its contents are not
                # a declaration. Keep the brace so the matching `}` does not pop
                # a scope that was never pushed.
                initializer += 1
            i += 1
            continue
        if char == "}" and paren == 0 and bracket == 0:
            if initializer:
                initializer -= 1
            elif scopes:
                scopes.pop()
            chunk = []
            i += 1
            continue
        if char == ";" and paren == 0 and bracket == 0 and initializer == 0:
            declaration = normalise("".join(chunk))
            chunk = []
            if declaration in ACCESS:
                if scopes and scopes[-1].kind == "type":
                    scopes[-1].access = declaration
            else:
                record(declaration, ";")
            i += 1
            continue
        if char == "," and paren == 0 and bracket == 0 and initializer == 0:
            if scopes and scopes[-1].kind == "enum":
                record(normalise("".join(chunk)), ",")
                chunk = []
                i += 1
                continue
        if char == ":" and paren == 0 and bracket == 0 and initializer == 0:
            declaration = normalise("".join(chunk))
            if declaration in ACCESS:
                if scopes and scopes[-1].kind == "type":
                    scopes[-1].access = declaration
                chunk = []
                i += 1
                continue
        chunk.append(char)
        i += 1
    return entries


def extract(paths, root=REPO_ROOT):
    """Set of snapshot lines for the given headers. Missing headers are an
    error: a gate that silently skips a file measures nothing."""
    entries = set()
    for relative in paths:
        try:
            with open(os.path.join(root, relative), encoding="utf-8") as handle:
                text = handle.read()
        except OSError as exc:
            raise SystemExit(f"api-snapshot: cannot read {relative}: {exc}")
        found = extract_header(text)
        if not found:
            raise SystemExit(f"api-snapshot: {relative} yielded no declarations")
        entries |= found
    return entries


def read_snapshot(path):
    """The committed entries, ignoring comments and blank lines."""
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().split("\n")
    except OSError as exc:
        raise SystemExit(f"api-snapshot: cannot read {path}: {exc}")
    # Comments are "# " (hash-space); a macro entry starts with "#define " and
    # is data, not a comment.
    return {line.strip() for line in lines
            if line.strip() and not line.lstrip().startswith("# ")}


def write_snapshot(path, entries):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# Public plugin API snapshot (source compatibility).\n")
        handle.write("# Generated by `python3 tools/api_snapshot.py --update`; "
                     "review the diff.\n")
        handle.write("# Every line must still exist; new lines are additive and "
                     "allowed.\n")
        for line in sorted(entries):
            handle.write(line + "\n")


def check(entries, snapshot, verbose=True, minimum=MIN_ENTRIES):
    """0 when every snapshot entry survives, 1 when one is missing (a removal,
    rename or signature change), 2 when the measurement itself is unusable.

    `minimum` is the measurement floor. The fixture tests pass 1 because a
    two-line header is a complete measurement for them; the real CLI keeps the
    floor that catches an extractor returning almost nothing.
    """
    if len(entries) < minimum:
        print(f"api-snapshot: FAILED CLOSED - only {len(entries)} public "
              f"declaration(s) found (floor {minimum}). The extractor is not "
              f"measuring the API.", file=sys.stderr)
        return 2
    if not snapshot:
        print("api-snapshot: FAILED CLOSED - the snapshot is empty; run "
              "`python3 tools/api_snapshot.py --update` and review it.",
              file=sys.stderr)
        return 2
    missing = sorted(snapshot - entries)
    added = sorted(entries - snapshot)
    if missing:
        print(f"api-snapshot: FAILED - {len(missing)} public declaration(s) "
              f"disappeared or changed:")
        for line in missing[:20]:
            print(f"  - {line}")
        if len(missing) > 20:
            print(f"  (+{len(missing) - 20} more)")
        print("  A removal or signature change needs a deprecation release and a "
              "migration; if the change is intentional, update "
              "`tests/api_snapshot.txt` in the same PR.")
        return 1
    if verbose:
        print(f"api-snapshot: {len(snapshot)} public declaration(s) intact"
              + (f", {len(added)} additive" if added else ""))
        if added:
            print("  review the additive entries and refresh the snapshot when "
                  "convenient: python3 tools/api_snapshot.py --update")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="compare with the snapshot (default)")
    ap.add_argument("--update", action="store_true", help="rewrite the snapshot")
    ap.add_argument("--headers", nargs="+", default=list(HEADERS),
                    help="header paths relative to --root")
    ap.add_argument("--root", default=REPO_ROOT, help="source root")
    ap.add_argument("--snapshot", default=SNAPSHOT, help="snapshot path")
    args = ap.parse_args()

    entries = extract(args.headers, args.root)
    if args.update:
        write_snapshot(args.snapshot, entries)
        print(f"api-snapshot: wrote {len(entries)} entries to {args.snapshot}")
        return 0
    return check(entries, read_snapshot(args.snapshot))


if __name__ == "__main__":
    sys.exit(main())
