#!/usr/bin/env python3
"""Enumerate everything a 0.9 capability row has to cover.

The journey matrix is only a coverage claim if the list of things to cover is
read from the product rather than remembered. This module is that list: the
documented command tree, the bundled plugin capabilities, the bare-host default
tool set, the TUI-only menu/keybinding actions and the five interface surfaces.
`tools/journey_matrix_check.py` holds the committed matrix to it.

The plugin/tool half is read from `tests/capability_snapshot.txt`, which
`make capability-snapshot-update` generates from the live plugin declarations
(and `make test` pins, so the snapshot cannot drift). The command half is read
from `completions.json`, the same file the drawer, help and dispatch use.

A parser that finds almost nothing would make the gate green on an empty
product, so every family carries a floor and a short scan exits non-zero.

Usage:
  tools/journey_sources.py --dump
  tools/journey_sources.py --dump --json
"""

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The interface regions the plan names. They are a fixed taxonomy rather than a
# source scan: there is no runtime registry of "surfaces", and inventing one to
# satisfy a checker would be the tail wagging the dog. A row per surface states
# which journey exercises it.
SURFACES = (
    "top-rail",
    "drawer",
    "editor",
    "dialog",
    "status-bar",
    "resource-viewer",
)

# Floors, not expectations: the measured tree is well above each one. A scan
# that loses the source file or changes shape trips a floor instead of reporting
# full coverage of nothing.
MIN_COMMANDS = 150
MIN_CAPABILITIES = 60
MIN_DEFAULT_TOOLS = 10
MIN_UI_ACTIONS = 5
MIN_CLI_JOURNEYS = 18
MIN_TUI_JOURNEYS = 14
MIN_LONG_HORIZON = 5
MIN_BENCH_SCENARIOS = 30

TUI_SELECTOR_RE = re.compile(r'std::string\(only\)\s*==\s*"([a-z0-9-]+)"')
TUI_DEFAULT_JOURNEY = "read-edit-verify"


@dataclass(frozen=True)
class Source:
    """One thing the matrix must account for."""

    ref: str
    kind: str
    detail: dict = field(default_factory=dict)


class ScanError(RuntimeError):
    """The tree did not look like the tree the gate is written against."""


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise ScanError(f"cannot read {path}: {error}") from error
    except ValueError as error:
        raise ScanError(f"invalid JSON in {path}: {error}") from error


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except OSError as error:
        raise ScanError(f"cannot read {path}: {error}") from error


def _require(condition, message):
    if not condition:
        raise ScanError(message)


def completion_paths(root=REPO_ROOT):
    """Every documented command path, with its action and help line.

    Paths, not actions: `/usage` and `/usage show` share an action, and a path is
    what a person types and what the drawer lists.
    """
    document = _read_json(os.path.join(root, "completions.json"))
    commands = document.get("commands")
    _require(isinstance(commands, dict) and commands, "completions.json has no commands")

    found = {}

    def walk(node, path):
        action = node.get("action")
        if action:
            found[path] = {"action": action, "help": node.get("help", "")}
        for name, child in node.get("children", {}).items():
            walk(child, f"{path}.{name}" if path else name)

    for name, node in commands.items():
        walk(node, name)
    _require(len(found) >= MIN_COMMANDS, f"completions.json yielded {len(found)} paths")
    return found


def completion_aliases(root=REPO_ROOT):
    """The visible root aliases and the documented path each dispatches into."""
    document = _read_json(os.path.join(root, "completions.json"))
    aliases = document.get("aliases", {})
    _require(isinstance(aliases, dict), "completions.json aliases is not an object")
    return dict(aliases)


def snapshot_capabilities(root=REPO_ROOT):
    """Every capability a bundled plugin declares, from the pinned snapshot."""
    path = os.path.join(root, "tests", "capability_snapshot.txt")
    found = {}
    for line in _read_text(path).splitlines():
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if fields[0] != "capability" or len(fields) != 4:
            continue
        _, plugin, kind, name = fields
        found[f"capability:{plugin}:{kind}:{name}"] = {
            "plugin": plugin,
            "capability_kind": kind,
            "name": name,
        }
    _require(len(found) >= MIN_CAPABILITIES, f"capability snapshot yielded {len(found)} rows")
    return found


def default_tools(root=REPO_ROOT):
    """Every tool the bare-host default composition registers."""
    path = os.path.join(root, "tests", "capability_snapshot.txt")
    found = {}
    for line in _read_text(path).splitlines():
        line = line.rstrip("\n")
        fields = line.split("\t")
        if len(fields) == 2 and fields[0] == "default-tool":
            found[f"default-tool:{fields[1]}"] = {"name": fields[1]}
    _require(len(found) >= MIN_DEFAULT_TOOLS, f"capability snapshot yielded {len(found)} tools")
    return found


def _action_set(path):
    document = _read_json(path)
    actions = set()

    def walk(node):
        if isinstance(node, dict):
            action = node.get("action")
            if isinstance(action, str):
                actions.add(action)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(document)
    return actions


def ui_actions(root=REPO_ROOT):
    """TUI-only actions: reachable from a menu or keybinding, not a command.

    A menu item that names a documented command action is covered by that
    command's row, so only the `ui.*` namespace is a separate source.
    """
    documented = {entry["action"] for entry in completion_paths(root).values()}
    actions = set()
    for name in ("menus.json", "keybindings.json"):
        actions |= _action_set(os.path.join(root, name))
    actions = {
        action
        for action in actions
        if action.startswith("ui.") and action not in documented
    }
    _require(len(actions) >= MIN_UI_ACTIONS, f"menu/keybinding scan yielded {len(actions)} actions")
    return {f"ui-action:{action}": {"action": action} for action in sorted(actions)}


def surface_sources():
    return {f"surface:{name}": {"name": name} for name in SURFACES}


def source_map(root=REPO_ROOT):
    """Every source indexed by its stable reference."""
    sources = {}
    for path, detail in completion_paths(root).items():
        sources[f"command:{path}"] = Source(f"command:{path}", "command", detail)
    for name, target in completion_aliases(root).items():
        sources[f"alias:{name}"] = Source(f"alias:{name}", "alias", {"target": target})
    for ref, detail in snapshot_capabilities(root).items():
        sources[ref] = Source(ref, "capability", detail)
    for ref, detail in default_tools(root).items():
        sources[ref] = Source(ref, "default-tool", detail)
    for ref, detail in ui_actions(root).items():
        sources[ref] = Source(ref, "ui-action", detail)
    for ref, detail in surface_sources().items():
        sources[ref] = Source(ref, "surface", detail)
    return sources


def cli_journeys(root=REPO_ROOT):
    """The CLI journey names the runner can execute."""
    baseline = _read_json(os.path.join(root, "bench", "results", "agent-e2e-baseline.json"))
    names = {row["name"] for row in baseline.get("rows", []) if row.get("name")}
    scenario_dir = os.path.join(root, "bench", "scenarios", "e2e")
    for entry in sorted(os.listdir(scenario_dir)):
        if not entry.endswith(".json"):
            continue
        scenario = _read_json(os.path.join(scenario_dir, entry))
        if scenario.get("name"):
            names.add(scenario["name"])
    _require(len(names) >= MIN_CLI_JOURNEYS, f"CLI journey scan yielded {len(names)}")
    return names


def tui_journeys(root=REPO_ROOT):
    """The TUI journey selectors, plus the unconditional default journey."""
    path = os.path.join(root, "tests", "tui_agent_e2e_test.cpp")
    source = _read_text(path)
    names = set(TUI_SELECTOR_RE.findall(source))
    _require(
        "journey_read_edit_verify" in source and TUI_DEFAULT_JOURNEY not in names,
        "the TUI default journey is missing from the runner",
    )
    names.add(TUI_DEFAULT_JOURNEY)
    _require(len(names) >= MIN_TUI_JOURNEYS, f"TUI journey scan yielded {len(names)}")
    return names


def long_horizon_journeys(root=REPO_ROOT):
    baseline = _read_json(os.path.join(root, "bench", "results", "long-horizon-baseline.json"))
    names = set(baseline.get("scenarios", {}))
    _require(len(names) >= MIN_LONG_HORIZON, f"long-horizon scan yielded {len(names)}")
    return names


def bench_journeys(root=REPO_ROOT):
    """Hermetic harness and long-horizon scenarios: the `bench` surface."""
    baseline = _read_json(
        os.path.join(root, "bench", "results", "harness-efficiency-baseline.json")
    )
    names = set(baseline.get("scenarios", {}))
    _require(len(names) >= MIN_BENCH_SCENARIOS, f"harness scan yielded {len(names)}")
    return names | long_horizon_journeys(root)


def journeys(root=REPO_ROOT):
    return {
        "cli": cli_journeys(root),
        "tui": tui_journeys(root),
        "bench": bench_journeys(root),
    }


def dump(root=REPO_ROOT):
    for ref in sorted(source_map(root)):
        print(ref)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump", action="store_true", help="print every source reference")
    parser.add_argument("--json", action="store_true", help="with --dump, emit kind and detail")
    parser.add_argument("--root", default=REPO_ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.dump:
            if args.json:
                payload = [
                    {"ref": source.ref, "kind": source.kind, "detail": source.detail}
                    for source in source_map(args.root).values()
                ]
                print(json.dumps(payload, indent=2, sort_keys=True))
            else:
                dump(args.root)
        else:
            counts = {}
            for source in source_map(args.root).values():
                counts[source.kind] = counts.get(source.kind, 0) + 1
            for name, members in journeys(args.root).items():
                counts[f"journey:{name}"] = len(members)
            print(json.dumps(counts, indent=2, sort_keys=True))
    except (OSError, ValueError, ScanError) as error:
        print(f"journey-sources: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
