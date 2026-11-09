#!/usr/bin/env python3
"""Fail-closed release gate for the live benchmark baseline.

A tagged engine release must carry a committed live run: a JSON report from
`amber-bench` whose `engine_version` is the tag, whose mode is `live`, whose
scenario count covers the model corpus, and whose scenarios all succeeded. An
engine-touching tag with no artifact is *not measured*, and that is a failure —
the one outcome this gate exists to prevent is a green release with no
measurement behind it.

Docs/website/packaging-only tags pass without a run: the changed-path allowlist
answers "is this an engine change?", and an unrecognised path counts as engine
(local parity with `tools/changed_paths.sh`).

The check is a ratchet against the previous release artifact, not a leaderboard:
a model score drop larger than `--band` on the same model/profile fails unless
the new artifact's `run_id` differs only because it is a new release (the band
is the recorded run-to-run variance, default 5 points on the 0..1000 score).
"""

import argparse
import json
import sys
from pathlib import Path

# Path prefixes that make a tag an engine release. Anything else (docs, website,
# packaging) may skip the live baseline; an unknown path is treated as engine so
# a new source directory cannot silently skip the gate.
ENGINE_PATHS = ("lib/", "src/", "tools/", "plugins/", "include/", "bench/", "prompts/")

# The model corpus the release procedure runs (`run_all_models.sh`, --cat model).
# Kept as a floor, not an equality: adding scenarios must not require editing the
# gate, while removing half the corpus must fail.
DEFAULT_MIN_SCENARIOS = 49
DEFAULT_BAND = 5.0


class Report:
    def __init__(self):
        self.code = 0
        self.lines = []

    def say(self, line):
        self.lines.append(line)

    def fail(self, line):
        self.say(line)
        self.code = 1


def engine_touching(paths):
    """True when any changed path is engine code; an unknown path is engine."""
    for path in paths:
        path = path.strip()
        if not path:
            continue
        if path.startswith(ENGINE_PATHS):
            return True
        # Documentation and website assets are the only legitimate skip. Any
        # other path is treated as engine so a new directory cannot slip past.
        if path.endswith(".md") or path.startswith("website/") or path.startswith(".github/"):
            continue
        if path.startswith("docs/") or path.startswith("packaging/"):
            continue
        if path in ("version.txt", "version.h.in") or path.startswith("bench/results/"):
            continue
        return True
    return False


def read_changed_paths(path):
    if not path:
        return None
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    return [line.strip() for line in text.splitlines() if line.strip()]


def corpus_count(corpus_dir):
    if not corpus_dir or not Path(corpus_dir).is_dir():
        return 0
    count = 0
    for path in Path(corpus_dir).rglob("*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and data.get("name") and data.get("prompt"):
            count += 1
    return count


def load_artifacts(results_dir):
    directory = Path(results_dir)
    if not directory.is_dir():
        return [], [f"no results directory: {results_dir}"]
    paths = sorted(directory.glob("*.json"))
    if not paths:
        return [], [f"no JSON artifact in {results_dir}"]
    artifacts = []
    errors = []
    for path in paths:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            errors.append(f"{path.name}: unreadable ({exc})")
            continue
        if not isinstance(data, dict):
            errors.append(f"{path.name}: not a JSON object")
            continue
        artifacts.append((path.name, data))
    return artifacts, errors


def validate_artifact(name, artifact, version, minimum_scenarios, model, profile):
    errors = []
    if artifact.get("mode") != "live":
        errors.append(f"{name}: mode is {artifact.get('mode')!r}, not 'live'")
    engine = str(artifact.get("engine_version", ""))
    if engine != version:
        errors.append(f"{name}: engine {engine or '(missing)'} != release {version}")
    if model and artifact.get("model") != model:
        errors.append(f"{name}: model {artifact.get('model')!r} != expected {model!r}")
    if profile and artifact.get("profile") != profile:
        errors.append(f"{name}: profile {artifact.get('profile')!r} != expected {profile!r}")

    scenarios = artifact.get("scenarios")
    if not isinstance(scenarios, list):
        errors.append(f"{name}: no scenarios array")
    else:
        if len(scenarios) < minimum_scenarios:
            errors.append(
                f"{name}: {len(scenarios)} scenarios, corpus floor is {minimum_scenarios}"
            )
        failed = [
            row.get("name", "?")
            for row in scenarios
            if isinstance(row, dict) and (row.get("success") is False or row.get("failures"))
        ]
        if failed:
            errors.append(f"{name}: {len(failed)} failed scenario(s): {', '.join(failed[:5])}")

    score = artifact.get("model_score")
    if not isinstance(score, (int, float)):
        errors.append(f"{name}: no numeric model_score")
    return errors, score


def previous_scores(previous_dir):
    """(model, profile) -> (filename, score, band from model_score_ci?)."""
    out = {}
    if not previous_dir:
        return out
    artifacts, _ = load_artifacts(previous_dir)
    for name, artifact in artifacts:
        score = artifact.get("model_score")
        if not isinstance(score, (int, float)):
            continue
        key = (artifact.get("model"), artifact.get("profile"))
        out[key] = (name, float(score))
    return out


def check(results_dir, version, corpus_dir=None, previous_dir=None, band=DEFAULT_BAND,
          model=None, profile=None, min_scenarios=None, changed_paths=None):
    report = Report()
    if changed_paths is not None and not engine_touching(changed_paths):
        report.say("bench: docs/packaging-only tag; live baseline not required")
        return report

    floor = min_scenarios if min_scenarios is not None else corpus_count(corpus_dir)
    if floor <= 0:
        floor = DEFAULT_MIN_SCENARIOS

    artifacts, errors = load_artifacts(results_dir)
    for error in errors:
        report.fail(f"bench: {error}")
    if not artifacts:
        report.fail(
            f"bench: NOT MEASURED - no live baseline for engine {version}; "
            "run the release benchmark and commit it under bench/results/releases/"
            f"{version}/"
        )
        return report

    previous = previous_scores(previous_dir)
    report.say(f"bench release check {version} (band {band:g}, scenarios >= {floor})")
    for name, artifact in sorted(artifacts):
        problems, score = validate_artifact(
            name, artifact, version, floor, model, profile
        )
        for problem in problems:
            report.fail(f"bench: {problem}")
        if problems:
            continue
        key = (artifact.get("model"), artifact.get("profile"))
        line = (
            f"  ok   {name}: engine {artifact.get('engine_version')} "
            f"{artifact.get('model')}/{artifact.get('profile') or 'default'} "
            f"score {score:.1f} ({len(artifact.get('scenarios', []))} scenarios)"
        )
        if key in previous:
            prev_name, prev_score = previous[key]
            drop = prev_score - float(score)
            if drop > band:
                report.fail(
                    f"bench: {name}: score {score:.1f} is {drop:.1f} below "
                    f"{prev_name} ({prev_score:.1f}), band {band:g}"
                )
            else:
                line += f" (vs {prev_name}: {score - prev_score:+.1f})"
        report.say(line)

    if report.code:
        report.say("bench: release baseline is NOT green")
    else:
        report.say("bench: release baseline green")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=None, help="release version (default: version.txt)")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--results-dir", default=None,
                        help="artifact directory (default: bench/results/releases/<version>)")
    parser.add_argument("--corpus-dir", default=None, help="scenario corpus root")
    parser.add_argument("--previous-dir", default=None, help="previous release artifacts")
    parser.add_argument("--band", type=float, default=DEFAULT_BAND)
    parser.add_argument("--model", default=None, help="expected model")
    parser.add_argument("--profile", default=None, help="expected profile")
    parser.add_argument("--min-scenarios", type=int, default=None)
    parser.add_argument(
        "--changed-paths-file",
        default=None,
        help="file of changed paths; docs/packaging-only skips the live gate",
    )
    args = parser.parse_args(argv)

    root = Path(args.root)
    version = args.version
    if not version:
        try:
            version = (root / "version.txt").read_text(encoding="utf-8").strip()
        except OSError:
            print("bench: cannot read version.txt", file=sys.stderr)
            return 2
    results_dir = args.results_dir or str(
        root / "bench" / "results" / "releases" / version
    )
    corpus_dir = args.corpus_dir or str(root / "bench" / "scenarios")
    report = check(
        results_dir,
        version,
        corpus_dir=corpus_dir,
        previous_dir=args.previous_dir,
        band=args.band,
        model=args.model,
        profile=args.profile,
        min_scenarios=args.min_scenarios,
        changed_paths=read_changed_paths(args.changed_paths_file),
    )
    for line in report.lines:
        print(line)
    return report.code


if __name__ == "__main__":
    sys.exit(main())
