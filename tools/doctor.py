#!/usr/bin/env python3
"""Report whether this machine can reproduce the gating CI checks.

Every gate fails closed when its analyzer is too *old*, but a floor check cannot
see the failure that costs the most round trips: the analyzer is **newer** than
the one CI runs, so it reports a different set of findings. That is not a
hypothetical: clang-tidy 21 and 18 disagree on whole check families, and
clang-format 18.1.8 and 18.1.3 disagree on braced-init and line-break
placement. A local run that is "clean" on a different toolchain is not evidence
about CI.

Doctor compares the tools the gating jobs use against the versions the workflow
pins, and says which ones cannot reproduce it:

    make doctor

Exit codes: 0 every gate can run (warnings allowed), 1 a gate cannot reproduce
CI, 2 the check itself could not run. A version that cannot be parsed is a
blocker, not a pass.

The pins mirror `.github/workflows/ci.yml` and the floors in `Makefile.in`;
`tests/gates/test_doctor.py` fails when the three drift apart. Phase 1 of
`docs/plan/pipeline-parity.md` replaces this duplication with a gate manifest.
"""

import os
import re
import shlex
import shutil
import subprocess
import sys

# CI pins. These are exact builds, not floors: a newer analyzer reports checks
# the pinned one does not, which is the drift this tool exists to name.
CI_PINS = {
    "clang-tidy": (18, 1, 3),
    "cppcheck": (2, 22, 0),
    "lizard": (1, 24, 0),
    "clang-format": (18, 1, 3),
    "gcovr": (8, 6),
}

# Floor variables the gates read from Makefile.in. Kept separately from CI_PINS
# because a floor and a pin are different contracts: the gate refuses an older
# tool, doctor refuses a tool that cannot reproduce the pinned one.
MAKEFILE_PINS = {
    "CLANG_TIDY_MIN_VERSION": "18.0.0",
    "CPPCHECK_MIN_VERSION": "2.22.0",
    "LIZARD_MIN_VERSION": "1.24.0",
    "CLANG_FORMAT_MIN_VERSION": "18.0.0",
}

# Needles that must survive a ci.yml edit, so a pin cannot move silently.
CI_PIN_NEEDLES = (
    "clang-tidy-18",
    "cppcheck=2.22.0",
    "lizard==1.24.0",
    "gcovr==8.6",
    "clang-format-18",
)

VERSION = re.compile(r"\d+(?:\.\d+)+")

OK, WARN, FAIL, UNKNOWN = "ok", "warn", "fail", "unknown"


def parse_version(text):
    """The first dotted version in a banner, or None when there is not one."""
    match = VERSION.search(text or "")
    if not match:
        return None
    return tuple(int(part) for part in match.group(0).split("."))


def classify(found, required, mode):
    """(status, note) for a probe result against a pinned version.

    mode:
      min   - found must be at least required; a newer major is drift, not an
              error, because the gate only refuses older tools.
      major - found must share required's major; a newer patch is a warning
              because the check set can still differ between builds.
      exact - found must equal required, otherwise a warning or a blocker.
    """
    if found is None:
        return FAIL, "not found"
    if mode == "min":
        if found < required:
            return FAIL, f"older than {'.'.join(map(str, required))}"
        if found[0] != required[0]:
            return WARN, f"CI runs {'.'.join(map(str, required))}"
        return OK, ""
    if mode == "major":
        if found[0] != required[0]:
            return FAIL, f"CI runs {'.'.join(map(str, required))} (different check set)"
        if found != required:
            return WARN, f"CI runs {'.'.join(map(str, required))}"
        return OK, ""
    if mode == "exact":
        if found == required:
            return OK, ""
        if found[0] == required[0]:
            return WARN, f"CI runs {'.'.join(map(str, required))}"
        return FAIL, f"CI runs {'.'.join(map(str, required))}"
    return UNKNOWN, f"unknown classification mode {mode!r}"


def exit_code(results):
    """0 only when no gate is blocked. Warnings are informative, not blockers."""
    return 1 if any(row["status"] in (FAIL, UNKNOWN) for row in results) else 0


def _command(parts):
    """Run a probe command, returning its combined output or None."""
    try:
        proc = subprocess.run(parts, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return (proc.stdout or "") + (proc.stderr or "")


def _env_command(name, default):
    """The executable a gate is configured to use (CLANG_TIDY, CPPCHECK, ...)."""
    raw = os.environ.get(name, "").strip()
    parts = shlex.split(raw) if raw else [default]
    return parts or [default]


def _probe(parts):
    if not parts:
        return None
    if shutil.which(parts[0]) is None and not os.path.isabs(parts[0]):
        return None
    return _command(parts + ["--version"])


def _compiler():
    raw = os.environ.get("CXX", "").strip()
    parts = shlex.split(raw) if raw else ["c++"]
    if len(parts) > 1:
        # CXX may be "ccache g++"; the compiler is the last token.
        parts = [parts[-1]]
    return parts


def check_results():
    """One row per tool that a gating job depends on."""
    rows = []

    compiler = _compiler()
    compiler_out = _probe(compiler)
    rows.append({
        "tool": "C++ compiler",
        "found": format_version(parse_version(compiler_out)) if compiler_out else None,
        "required": "present",
        "status": OK if compiler_out else FAIL,
        "note": "" if compiler_out else f"{compiler[0]} not found",
    })

    tidy = _env_command("CLANG_TIDY", "clang-tidy")
    rows.append(_row("clang-tidy", _probe(tidy), CI_PINS["clang-tidy"], "major",
                     "install clang-tidy-18 (apt) or run in the parity container"))

    cppcheck = _env_command("CPPCHECK", "cppcheck")
    rows.append(_row("cppcheck", _probe(cppcheck), CI_PINS["cppcheck"], "min",
                     "conda-forge cppcheck=2.22.0"))

    lizard_python = os.environ.get("COMPLEXITY_PYTHON", sys.executable or "python3")
    lizard = _probe([lizard_python, "-m", "lizard"])
    rows.append(_row("lizard", lizard, CI_PINS["lizard"], "min",
                     f'pip install "lizard=={format_version(CI_PINS["lizard"])}"'))

    fmt = _env_command("CLANG_FORMAT", "clang-format")
    rows.append(_row("clang-format", _probe(fmt), CI_PINS["clang-format"], "exact",
                     "pip install clang-format==18.1.3 (CI uses Ubuntu's 18.1.3)"))

    rows.append(_row("gcovr", _probe(["gcovr"]), CI_PINS["gcovr"], "min",
                     f'pip install "gcovr=={format_version(CI_PINS["gcovr"])}"'))

    for name in ("make", "python3", "git", "pkg-config"):
        path = shutil.which(name)
        rows.append({
            "tool": name,
            "found": "present" if path else None,
            "required": "present",
            "status": OK if path else FAIL,
            "note": "" if path else "not on PATH",
        })
    return rows


def _row(name, output, required, mode, remedy):
    found = parse_version(output) if output else None
    status, note = classify(found, required, mode)
    if status == FAIL and remedy:
        note = f"{note} — {remedy}" if note else remedy
    return {
        "tool": name,
        "found": format_version(found) if found else None,
        "required": format_version(required),
        "status": status,
        "note": note,
    }


def format_version(version):
    return None if version is None else ".".join(str(part) for part in version)


def render(rows):
    """A fixed-width table, because the answer is read at a glance."""
    headers = ("tool", "found", "required", "status", "note")
    widths = [len(h) for h in headers]
    for row in rows:
        for index, key in enumerate(headers):
            widths[index] = max(widths[index], len(str(row.get(key) or "")))
    lines = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    lines.append("  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rows:
        cells = [str(row.get(key) or "-") for key in headers]
        lines.append("  ".join(cells[i].ljust(widths[i]) for i in range(len(cells))).rstrip())
    return "\n".join(lines)


def main():
    rows = check_results()
    print(render(rows))
    blockers = sum(1 for row in rows if row["status"] in (FAIL, UNKNOWN))
    warnings = sum(1 for row in rows if row["status"] == WARN)
    if blockers:
        print(f"\ndoctor: {blockers} blocker(s), {warnings} warning(s); "
              "these gates cannot reproduce CI on this machine")
    else:
        print(f"\ndoctor: ready ({warnings} warning(s))")
    return exit_code(rows)


if __name__ == "__main__":
    sys.exit(main())
