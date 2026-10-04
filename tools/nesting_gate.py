#!/usr/bin/env python3
"""Nesting-depth gate (ratchet).

A function that nests deeply is hard to read no matter how short it is: the
reader must hold every enclosing level in mind to know what an `else` or a
`}` belongs to. This is a separate axis from NLOC and CCN for exactly that
reason — a 20-line function can be unreadable at five levels of nesting while a
40-line function of straight-line code is fine.

**Why not lizard.** lizard's `-w` output ends with an `ND` field
(`max_nesting_depth`) and it looks like it would answer this. It does not: for
C++ that field is 0 for every function. Verified against a deliberately 6-deep
function, which lizard still reports as `0 ND`. Gating on it would produce a
gate that is permanently green because it measures nothing — precisely the
failure mode the other three gates were fixed to reject. So nesting is measured
here, from the source, using `tools/cpp_source.py`.

**What counts.** The maximum number of nested *control structures*
(`if`/`else`/`for`/`while`/`switch`/`do`/`try`/`catch`), where a control
structure's own body is one level and the enclosing function is not. This matches
the common industry definitions (SonarQube's nesting depth, ESLint's max-depth).
Anything that is not a control structure does not nest: a struct body, a braced
initializer, a lambda body, a ternary.

**Fails closed**, like the other gates: if a file's braces do not balance the
lexer cannot trust its own output, and a file that cannot be measured must be an
error rather than a silent zero. That happens for real code — a preprocessor
conditional can leave a brace compiled out — so the gate reports which files it
could not read instead of scoring them zero.

Usage:
  tools/nesting_gate.py --report    # list the violations (informational)
  tools/nesting_gate.py --check     # gate against the baseline (CI)
  tools/nesting_gate.py --update    # rewrite the baseline after flattening
"""

import argparse
import json
import os
import re
import subprocess
import sys

import cpp_source

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 4 is the value most tools settle on: ESLint's max-depth defaults to 4, and
# SonarQube's nesting rule defaults to 3 for a single method. Anything deeper than
# four levels of control flow is hard to hold in the head.
NESTING_MAX = 4

# A healthy scan of this tree sees thousands of functions. As in
# complexity_gate.py this floor exists only to catch a scan that measured nothing.
MIN_FUNCTIONS_SANE = 200

BASELINE = os.path.join(REPO_ROOT, "tests", "nesting_baseline.json")

ROOTS = ["lib", "tools", "tui", "src", "bench", "plugins"]

# Keywords that open a nesting level when they are followed by a block.
CONTROL_KEYWORDS = frozenset(
    {"if", "else", "for", "while", "switch", "do", "try", "catch"})

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# lizard reports a `catch` block of a function-try-block as if it were a function
# of its own ("catch", length 4). They are not functions and have no signature to
# measure, so they are dropped rather than reported as unmeasurable.
NOT_A_FUNCTION = CONTROL_KEYWORDS

WARN_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+): warning: (?P<name>.+?) has "
    r"(?P<nloc>\d+) NLOC, (?P<ccn>\d+) CCN, (?P<token>\d+) token, "
    r"(?P<param>\d+) PARAM, (?P<length>\d+) length"
)
LOOSE_RE = re.compile(r"^[^:]+:\d+: warning: ")


def strip_noise(source):
    """Exposed for the tests and for callers that already hold the text."""
    return cpp_source.strip_noise(source)


def baseline_path():
    return BASELINE


def lizard_command():
    # Same exclusions as complexity_gate.py, and for the same reasons: vendored
    # code is not ours, bench/results is generated, and bench/scenarios is fixture
    # code the benchmark agent writes rather than project style. As there, the glob
    # form is the one lizard actually matches.
    return [sys.executable, "-m", "lizard", "-w", "-l", "cpp", "-C", "1", "-L", "1",
            "--exclude", "third_party", "--exclude", "bench/results",
            "--exclude", "*/scenarios/*", *ROOTS]


def parse_lizard_output(lines):
    """(functions, unparsed_count). See complexity_gate.py for why unparsed is
    counted rather than dropped: a lizard release that moves its -w layout must be
    a loud failure, not an empty result."""
    functions, unparsed = [], 0
    for raw in lines:
        line = raw.strip()
        match = WARN_RE.match(line)
        if match:
            if match.group("name") in NOT_A_FUNCTION:
                continue
            functions.append({
                "file": match.group("file"),
                "line": int(match.group("line")),
                "name": match.group("name"),
                "length": int(match.group("length")),
            })
        elif LOOSE_RE.match(line):
            unparsed += 1
    return functions, unparsed


def run_lizard():
    """(functions, None) or (None, reason). The exit status is ignored on purpose:
    `-C 1 -L 1` makes every function a warning, so lizard exits 1 on a good run."""
    try:
        proc = subprocess.run(lizard_command(), capture_output=True, text=True)
    except OSError as exc:
        return None, f"could not run lizard: {exc}"
    return validate_scan(*parse_lizard_output(proc.stdout.splitlines()))


def validate_scan(functions, unparsed):
    if unparsed:
        return None, (f"{unparsed} lizard output line(s) did not match the expected "
                      f"-w format; lizard's output layout changed")
    if not functions:
        return None, "lizard reported no functions"
    return functions, None


def starts_a_statement(line, at):
    """True when the identifier at `at` begins a statement rather than continuing
    one: nothing but whitespace before it, and not a member access (`obj.for_each`
    is not a for statement)."""
    for char in reversed(line[:at]):
        if char.isspace():
            continue
        return char not in ".>"
    return True


def max_nesting(lines, start=0, end=None):
    """Deepest chain of nested control structures in `lines`, counting from `start`.

    A level opens when a control keyword's block follows. A keyword with no block
    (`if (a) b();`) opens nothing, and neither does anything that is not a control
    structure — a struct body, a braced initializer, a lambda body, a ternary.
    `else if` opens one level, not two.

    Depth counts enclosing *control* blocks, not braces: the enclosing function's
    own brace, and a lambda's or a struct's, must not make the number mean
    something other than how deeply the control flow is nested.
    """
    stop = len(lines) if end is None else end
    stack = []        # one entry per open brace: did a control keyword open it?
    pending = False   # a control keyword is waiting for its block
    carried = False   # that keyword was seen on the previous line
    parens = 0
    control_depth = 0
    deepest = 0

    for line in lines[start:stop]:
        depth_here = deepest
        opened = False
        pos = 0
        while pos < len(line):
            word = IDENT.match(line, pos)
            if word:
                if word.group(0) in CONTROL_KEYWORDS and starts_a_statement(line, pos):
                    pending = True
                pos = word.end()
                continue
            char = line[pos]
            if char == "(":
                parens += 1
            elif char == ")":
                parens = max(0, parens - 1)
            elif char == "{":
                stack.append(pending)
                if pending:
                    control_depth += 1
                    depth_here = max(depth_here, control_depth)
                pending, opened = False, True
            elif char == "}":
                if stack and stack.pop():
                    control_depth = max(0, control_depth - 1)
                pending = False
            elif char == ";" and parens == 0:
                pending = False   # `if (a) ;` — but not the `;;` of `for (;;)`
            pos += 1
        # A keyword whose block never appears must not claim the next unrelated
        # brace, so `pending` survives at most one line.
        if pending and not opened:
            pending, carried = carried, True
        else:
            carried = False
        deepest = depth_here
    return deepest


def continues_into_catch(line):
    """True when the text after the brace that just closed is a `catch`.

    C++ function-try-blocks — `int main() try { ... } catch (...) { ... }` — close
    the try body and immediately open the handler, so depth returns to zero in the
    middle of the function. Every `main` in this repo is written that way.
    """
    tail = line.rsplit("}", 1)[-1].strip()
    return tail.startswith("catch")


def function_extent(lines, start):
    """(end_index_exclusive, balanced) for the function whose body opens at or
    after `start`.

    lizard's reported `length` is not used: it undercounts a function-try-block,
    because it stops where the try body closes. Deriving the extent here also means
    a lizard release changing that number cannot silently move every measurement.
    """
    depth, seen = 0, False
    for i in range(start, len(lines)):
        line = lines[i]
        opens = line.count("{")
        depth += opens - line.count("}")
        # A body written entirely on one line (`... : x_(y) {}`) opens and closes
        # without the running depth ever exceeding zero, so `seen` tracks the
        # opening brace rather than the depth.
        if opens:
            seen = True
        if not seen or depth > 0:
            continue
        if continues_into_catch(line):
            seen = False
            continue
        return i + 1, True
    return len(lines), False


def nesting_by_file(functions):
    """(depths keyed by (file, name), functions that could not be measured).

    A function is measured over its own extent. Anything that does not close
    cleanly is reported rather than scored, because a wrong zero understates it —
    that is the failure mode this gate exists to reject, not to reproduce.

    Two functions in one file can share a name (overloads, or a name reused in an
    anonymous namespace), and the baseline is keyed by (file, name) so that
    renumbering lines does not churn it. That makes collisions possible, and the
    shallower result must not win: the deepest is kept, so a deep overload cannot
    hide behind a shallow one.
    """
    cache, depths, unmeasurable = {}, {}, {}
    for entry in functions:
        path = entry["file"]
        if path not in cache:
            cache[path] = read_lines(path)
        lines = cache[path]
        key = (path, entry["name"])
        if lines is None:
            unmeasurable[key] = "file could not be read"
            continue
        start = max(entry["line"] - 1, 0)
        end, balanced = function_extent(lines, start)
        if not balanced:
            unmeasurable[key] = f"no closing brace from line {entry['line']}"
            continue
        depth = max_nesting(lines, start=start, end=end)
        depths[key] = max(depths.get(key, 0), depth)
    for key in unmeasurable:
        depths.pop(key, None)
    return depths, unmeasurable


def read_lines(path):
    """Structural lines for a file: comments and literals blanked.

    This must be the *structural* text, not the raw source. Running the walker on
    raw text counts braces and keywords inside strings and comments, which both
    invents nesting and reports perfectly ordinary files as unbalanceable.
    """
    if not os.path.isabs(path):
        path = os.path.join(REPO_ROOT, path)
    source, structural = cpp_source.read(path)
    if structural is None:
        return None
    return structural.split("\n")


def violations_of(depths):
    return {key: depth for key, depth in depths.items() if depth > NESTING_MAX}


def report(functions, depths, unmeasurable):
    print(f"functions scanned: {len(functions)}")
    print(f"measured: {len(depths)}   unmeasurable: {len(unmeasurable)}")
    over = violations_of(depths)
    print(f"over-limit (nesting > {NESTING_MAX}): {len(over)}")
    for (path, name), depth in sorted(over.items(), key=lambda kv: (-kv[1], kv[0]))[:15]:
        print(f"    {depth:>3}  {path}  {name}")
    for (path, name), why in sorted(unmeasurable.items())[:15]:
        print(f"    ??  {path}  {name}  ({why})")


def keyed(over):
    return dict(over)


def load_baseline():
    """The recorded depths, or None when there is no baseline file at all.

    An empty baseline is the state this ratchet works toward, not a missing file.
    """
    if not os.path.exists(BASELINE):
        return None
    with open(BASELINE, encoding="utf-8") as handle:
        raw = json.load(handle)
    return {(path, name): raw["depths"][path][name]
            for path, names in raw.get("depths", {}).items()
            for name in names}


def check(over, measured, unmeasurable):
    """`measured` and `unmeasurable` are required: a caller that forgets to pass
    the scan's liveness signals must not inherit a silent pass."""
    baseline = load_baseline()
    if baseline is None:
        print(f"nesting: no baseline at {BASELINE}; run --update")
        return 2
    if unmeasurable:
        print(f"nesting: FAILED CLOSED - {len(unmeasurable)} function(s) could not be "
              "measured because their braces do not balance over the range lizard "
              "reports. Scoring them zero would understate them:")
        for (path, name), why in sorted(unmeasurable.items())[:20]:
            print(f"  {path}: {name} ({why})")
        return 2
    if measured < MIN_FUNCTIONS_SANE:
        print(f"nesting: FAILED CLOSED - measured only {measured} function(s), expected at "
              f"least {MIN_FUNCTIONS_SANE}. Check ROOTS and the --exclude list.")
        return 2

    current = keyed(over)
    new = [k for k in sorted(current) if k not in baseline]
    grew = [k for k in sorted(current)
            if k in baseline and current[k] > baseline[k]]

    if new or grew:
        print_failures(new, grew, baseline, current)
        return 1

    shrunk = [k for k in baseline if k not in current]
    if shrunk:
        print(f"nesting: {len(shrunk)} baselined function(s) are now within limits - "
              f"run `make nesting-update` to lock that in")
    print(f"  ok: {len(current)} over-limit function(s), none added or grown "
          f"(baseline holds)")
    return 0


def print_failures(new, grew, baseline, current):
    if new:
        print(f"nesting: {len(new)} new over-limit function(s):")
        for path, name in new:
            print(f"  {path}: {name} nests {current[(path, name)]} deep "
                  f"(limit {NESTING_MAX})")
    if grew:
        print(f"nesting: {len(grew)} function(s) got deeper:")
        for path, name in grew:
            print(f"  {path}: {name} {baseline[(path, name)]} -> {current[(path, name)]} "
                  f"(limit {NESTING_MAX})")
    print("  flatten the nesting (early return / extract a helper), or justify it in "
          "the baseline via --update.")


def by_file(depths):
    grouped = {}
    for (path, name), depth in sorted(depths.items()):
        grouped.setdefault(path, {})[name] = depth
    return grouped


def update(depths):
    payload = {
        "nesting_max": NESTING_MAX,
        "depths": by_file(depths),
    }
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"nesting: baseline written to {BASELINE} ({len(depths)} functions)")
    return 0


def discard_baseline():
    """Used by the tests: remove a baseline this run created."""
    if os.path.exists(BASELINE):
        os.unlink(BASELINE)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="gate against the baseline")
    ap.add_argument("--report", action="store_true", help="print the violations")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    args = ap.parse_args()

    functions, err = run_lizard()
    if err:
        print(f"nesting: FAILED CLOSED - {err}", file=sys.stderr)
        print("nesting: install it with 'pip install lizard'", file=sys.stderr)
        return 2

    depths, unmeasurable = nesting_by_file(functions)
    if unmeasurable:
        print(f"nesting: FAILED CLOSED - {len(unmeasurable)} function(s) could not be "
              f"measured; run --report for the list", file=sys.stderr)
        return 2
    over = violations_of(depths)
    if args.update:
        return update(over)
    report(functions, depths, unmeasurable)
    print()
    return check(over, len(depths), unmeasurable)


if __name__ == "__main__":
    sys.exit(main())