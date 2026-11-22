#!/usr/bin/env python3
"""Check the 0.9 capability matrix against the product it claims to cover.

`tests/journey_matrix.json` is the one answer to "what is covered?". This gate
makes it load-bearing:

- every source in `tools/journey_sources.py` (documented command path, visible
  alias, bundled plugin capability, bare-host default tool, TUI-only action and
  interface surface) appears in exactly one row;
- no row names a source that does not exist, and no source is named twice;
- every journey cell is a journey the corresponding runner can execute, or an
  explicit `internal:` / `na:` / `deferred:` reason;
- a `deferred: #issue <reason>` cell is a recorded gap, not a pass: the 0.9
  release check runs with `--require-complete`, which refuses every one.

The gate reads the tree, never a PR body. A source scan that finds almost
nothing exits 2 (measurement failure) rather than reporting full coverage of an
empty product; `journey_sources` carries the floors.

Usage:
  python3 tools/journey_matrix_check.py
  python3 tools/journey_matrix_check.py --require-complete
  python3 tools/journey_matrix_check.py --report
"""

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

import journey_sources

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST_PATH = os.path.join("tests", "journey_matrix.json")

# The matrix is the coverage surface for 59+ capability groups; a scan that
# finds a handful is a broken measurement, not a smaller product.
MIN_ROWS = 40
MIN_SOURCES = 250

SURFACES = ("cli", "tui", "bench")
ROW_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
DEFERRED_RE = re.compile(r"^deferred:\s*#(\d+)\s+(\S.*)$")


@dataclass
class Report:
    """The gate's verdict. `code` is 0 pass, 1 finding, 2 measurement error."""

    code: int = 0
    lines: list = field(default_factory=list)
    counts: dict = field(default_factory=dict)

    def fail(self, message):
        self.code = max(self.code, 1)
        self.lines.append(f"FAIL: {message}")

    def error(self, message):
        self.code = 2
        self.lines.append(f"ERROR: {message}")

    def note(self, message):
        self.lines.append(message)


def _reasoned(cell, prefix, report, where):
    """`prefix: reason` with a reason that says something."""
    reason = cell[len(prefix) :].strip()
    if len(reason) < 8:
        report.fail(f"{where}: '{cell}' needs a reason, not a placeholder")
        return False
    return True


def _check_cell(cell, surface, journeys, report, where, require_complete):
    if not isinstance(cell, str) or not cell.strip():
        report.fail(f"{where}: empty {surface} cell")
        return
    text = cell.strip()
    if text.startswith("internal:"):
        _reasoned(text, "internal:", report, where)
        report.counts["internal"] += 1
        return
    if text.startswith("na:"):
        _reasoned(text, "na:", report, where)
        report.counts["na"] += 1
        return
    if text.startswith("deferred:"):
        match = DEFERRED_RE.match(text)
        if not match:
            report.fail(f"{where}: deferred cell needs '#issue <reason>': {text}")
            return
        _reasoned(text, f"deferred: #{match.group(1)}", report, where)
        report.counts["deferred"] += 1
        if require_complete:
            report.fail(f"{where}: deferred gap remains ({text})")
        return
    if text not in journeys[surface]:
        report.fail(f"{where}: unknown {surface} journey '{text}'")
        return
    report.counts["journey"] += 1


def _cells(value, surface, report, where):
    if isinstance(value, list):
        if not value:
            report.fail(f"{where}: {surface} has an empty list")
        return value
    return [value]


def _check_row(row, journeys, report, require_complete):
    if not isinstance(row, dict):
        report.fail("a capability row is not an object")
        return None
    row_id = row.get("id")
    if not isinstance(row_id, str) or not ROW_ID_RE.match(row_id):
        report.fail(f"invalid capability id {row_id!r}")
        return None
    sources = row.get("sources")
    if not isinstance(sources, list) or not sources:
        report.fail(f"{row_id}: sources must be a non-empty list")
        return None
    for ref in sources:
        if not isinstance(ref, str) or not ref:
            report.fail(f"{row_id}: a source reference is not a string")
    cells = row.get("journeys")
    if not isinstance(cells, dict) or set(cells) != set(SURFACES):
        report.fail(f"{row_id}: journeys must have exactly {', '.join(SURFACES)}")
        return None
    for surface in SURFACES:
        for cell in _cells(cells[surface], surface, report, f"{row_id}.{surface}"):
            _check_cell(cell, surface, journeys, report, f"{row_id}.{surface}", require_complete)
    notes = row.get("notes", "")
    if notes is not None and not isinstance(notes, str):
        report.fail(f"{row_id}: notes must be a string")
    return row_id


def _check_surfaces(document, report):
    declared = document.get("surfaces")
    if not isinstance(declared, list):
        report.error("the matrix has no surfaces list")
        return
    ids = []
    for entry in declared:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            report.error("a surface entry is not {id, source, description}")
            continue
        ids.append(entry["id"])
    if len(ids) != len(set(ids)):
        report.error("the surfaces list repeats an id")
    if set(ids) != set(journey_sources.SURFACES):
        report.error(
            "surfaces do not match tools/journey_sources.py: "
            f"declared={sorted(ids)} expected={sorted(journey_sources.SURFACES)}"
        )


def _target_path(alias_detail, known):
    target = alias_detail.get("target", "").replace(" ", ".")
    while target and target not in known:
        target = target.rsplit(".", 1)[0] if "." in target else ""
    return target


def check(root=REPO_ROOT, require_complete=False):
    report = Report(counts={"journey": 0, "internal": 0, "na": 0, "deferred": 0})
    try:
        index = journey_sources.journeys(root)
        all_sources = journey_sources.source_map(root)
    except (OSError, ValueError, journey_sources.ScanError) as error:
        report.error(f"source scan failed: {error}")
        return report

    if len(all_sources) < MIN_SOURCES:
        report.error(f"source scan yielded {len(all_sources)} sources")
        return report
    for surface in SURFACES:
        if not index[surface]:
            report.error(f"the {surface} journey index is empty")
    if report.code == 2:
        return report

    manifest_path = os.path.join(root, MANIFEST_PATH)
    try:
        with open(manifest_path, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as error:
        report.error(f"cannot read {MANIFEST_PATH}: {error}")
        return report

    if document.get("version") != 1:
        report.error("the matrix version must be 1")
    _check_surfaces(document, report)
    rows = document.get("capabilities")
    if not isinstance(rows, list):
        report.error("the matrix has no capabilities list")
        return report
    if len(rows) < MIN_ROWS:
        report.error(f"the matrix has {len(rows)} rows")

    owner = {}
    row_ids = set()
    for row in rows:
        row_id = _check_row(row, index, report, require_complete)
        if row_id is None:
            continue
        if row_id in row_ids:
            report.fail(f"duplicate capability id '{row_id}'")
            continue
        row_ids.add(row_id)
        for ref in row["sources"]:
            if ref not in all_sources:
                report.fail(f"{row_id}: unknown source '{ref}'")
                continue
            if ref in owner:
                report.fail(f"source '{ref}' is in both '{owner[ref]}' and '{row_id}'")
                continue
            owner[ref] = row_id

    missing = sorted(set(all_sources) - set(owner))
    if missing:
        report.fail(f"{len(missing)} source(s) are in no capability row")
        for ref in missing[:20]:
            report.note(f"  missing: {ref}")
        if len(missing) > 20:
            report.note(f"  ... and {len(missing) - 20} more")

    # An alias is a second spelling of its target, so the journey that covers
    # the target must cover the alias in the same row. A row that splits them
    # would claim coverage the evidence does not reach. The one exception is a
    # visible alias whose target is a dynamically contributed subtree (the
    # MCP plugin's `/plugin mcp`): the alias owner names the capability row that
    # has the journey, because the static tree only records the `/plugin` root.
    documented = set(journey_sources.completion_paths(root))
    alias_owners = document.get("alias_owners", {})
    if not isinstance(alias_owners, dict):
        report.error("alias_owners must be an object")
        alias_owners = {}
    for name, row_id in alias_owners.items():
        if row_id not in row_ids:
            report.fail(f"alias_owners.{name} names unknown row '{row_id}'")
    for ref in sorted(owner):
        if not ref.startswith("alias:"):
            continue
        detail = all_sources[ref].detail
        target = _target_path(detail, documented)
        if not target:
            report.fail(f"{owner[ref]}: alias '{ref}' has no documented target")
            continue
        name = ref[len("alias:") :]
        if name in alias_owners:
            if alias_owners[name] != owner[ref]:
                report.fail(f"alias '{ref}' is not in its alias_owners row '{alias_owners[name]}'")
            continue
        target_row = owner.get(f"command:{target}")
        if target_row != owner[ref]:
            report.fail(
                f"alias '{ref}' is in '{owner[ref]}' but its target "
                f"'command:{target}' is in '{target_row}'"
            )

    report.counts["rows"] = len(row_ids)
    report.counts["sources"] = len(all_sources)
    if report.code == 0:
        report.note(
            f"journey matrix green: {len(row_ids)} capabilities, {len(all_sources)} sources, "
            f"{report.counts['journey']} journey cells, {report.counts['internal']} internal, "
            f"{report.counts['na']} na, {report.counts['deferred']} deferred"
        )
    return report


def _report_table(report):
    report.note("counts: " + ", ".join(f"{k}={v}" for k, v in sorted(report.counts.items())))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help="refuse every deferred cell; this is the 0.9 release check",
    )
    parser.add_argument("--report", action="store_true", help="print the count table")
    parser.add_argument("--root", default=REPO_ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    report = check(args.root, require_complete=args.require_complete)
    for line in report.lines:
        print(line)
    if args.report:
        _report_table(report)
    return report.code


if __name__ == "__main__":
    sys.exit(main())
