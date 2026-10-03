#!/usr/bin/env python3
"""Run every gate, collect the verdicts, emit one Markdown and one JSON summary.

The gates already print their verdicts, but into a dozen separate job logs. The
question a reviewer actually asks is "what did all the gates say?", and answering
it should not mean opening six runs and grepping. This runs them, records each
verdict, and writes:

  artifacts/GATE_REPORT.md    the table a human reads
  artifacts/gate-report.json   the same, machine-readable

CI uploads both as a build artefact — GitHub's equivalent of an ADO build
artefact. They are attached to the run, listed on the run page, and downloadable
with `gh run download <id>`, so a gate's verdict can be reviewed without
re-running anything locally.

**Verdicts are not exit codes.** A gate that found a violation exits 1; a gate
whose *measurement* degraded exits 2; both are failures, and a summary that called
them the same thing would hide which is which. `error` is reserved for the second,
because it means the gate did not actually measure anything.

Usage:
  tools/gate_report.py --out artifacts
  tools/gate_report.py --out artifacts --only complexity nesting
"""

import argparse
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PASS, FAIL, ERROR, SKIP = "pass", "fail", "error", "skipped"

# Every gate that can run without a build. Kept explicit rather than discovered,
# so adding a gate is a visible edit and a missing entry is a review question.
COMMANDS = {
    "complexity": ["make", "complexity"],
    "nesting": ["make", "nesting"],
    "class-size": ["python3", "tools/class_size_gate.py", "--check"],
    "debt": ["make", "debt"],
    "obfuscation": ["python3", "tools/obfuscation_guard.py"],
    "duplicates": ["make", "duplicates"],
}

GATES = tuple(COMMANDS)

MAX_DETAIL_LINES = 40


def verdict_for(code):
    """Map an exit status to a verdict. Anything unexpected (a signal, or a
    command that could not be found) is an error, not a silent skip."""
    if code == 0:
        return PASS
    if code == 1:
        return FAIL
    return ERROR


def exit_code(results):
    """0 only when nothing failed. A skipped gate does not hide a failure."""
    if any(r["verdict"] in (FAIL, ERROR) for r in results):
        return 1 if all(r["verdict"] == FAIL for r in results) else 2
    return 0


def command_for(name):
    if name not in COMMANDS:
        raise KeyError(f"unknown gate: {name}")
    return list(COMMANDS[name])


def summarise(text):
    """The line that carries the verdict, so the table is readable at a glance."""
    for line in reversed(text.splitlines()):
        stripped = line.strip()
        if stripped and not stripped.startswith(("#", "make[")):
            return stripped[:160]
    return ""


def run(name, command):
    try:
        proc = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True)
    except OSError as exc:
        return {"gate": name, "verdict": ERROR, "summary": f"could not run: {exc}",
                "details": ""}
    output = (proc.stdout or "") + (proc.stderr or "")
    details = "\n".join(output.splitlines()[-MAX_DETAIL_LINES:])
    return {"gate": name, "verdict": verdict_for(proc.returncode),
            "summary": summarise(output), "details": details}


def cell(text):
    """Table-safe: a `|` or newline in gate output must not break the table."""
    return str(text).replace("|", "\\|").replace("\n", "<br>")


def to_markdown(results):
    verdicts = {r["verdict"] for r in results}
    overall = "FAIL" if FAIL in verdicts or ERROR in verdicts else "PASS"
    lines = [
        "# Gate report",
        "",
        f"**Overall: {overall}** "
        f"({sum(1 for r in results if r['verdict'] == PASS)} pass, "
        f"{sum(1 for r in results if r['verdict'] == FAIL)} fail, "
        f"{sum(1 for r in results if r['verdict'] == ERROR)} error)",
        "",
        "| gate | verdict | summary |",
        "|---|---|---|",
    ]
    for r in results or [{"gate": "-", "verdict": SKIP, "summary": "none were run"}]:
        lines.append(f"| {cell(r['gate'])} | {cell(r['verdict'])} | {cell(r['summary'])} |")
    detailed = [r for r in results if r.get("details")]
    if detailed:
        lines += ["", "## Detail", ""]
        for r in detailed:
            lines += [f"### {r['gate']} ({r['verdict']})", "", "```", r["details"], "```", ""]
    return "\n".join(lines) + "\n"


def to_json(results):
    return json.dumps(sorted(results, key=lambda r: r["gate"]), indent=2,
                      sort_keys=True) + "\n"


def write_outputs(results, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, text in (("GATE_REPORT.md", to_markdown(results)),
                       ("gate-report.json", to_json(results))):
        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
    print(f"gate-report: wrote {out_dir}/GATE_REPORT.md and {out_dir}/gate-report.json")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="artifacts", help="directory for the two reports")
    ap.add_argument("--only", nargs="*", choices=GATES, help="run a subset")
    args = ap.parse_args()

    names = args.only or list(GATES)
    results = []
    for name in names:
        print(f"gate-report: running {name}...", file=sys.stderr)
        results.append(run(name, command_for(name)))
    write_outputs(results, args.out)
    for r in results:
        print(f"  {r['verdict']:<7} {r['gate']:<12} {r['summary']}")
    return exit_code(results)


if __name__ == "__main__":
    sys.exit(main())