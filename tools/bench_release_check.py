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

The same comparison enforces the cost and behaviour bands on the scenarios
successful in both artifacts: prompt+completion tokens and tool calls within
+10% in aggregate, no compaction failure, no hard stop or wall-budget failure,
and a per-scenario wall/TTFT pathology ceiling. The bands are reviewed before
the run and documented in `docs/release-benchmark.md`.
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
DEFAULT_COST_BAND = 0.10
DEFAULT_COMPARABLE_FLOOR = 30
# A pathology ceiling, not a micro-regression: live endpoints have tail
# latency. A scenario that takes three times as long (and at least a minute
# longer) or whose first token is three times later (and 10 s later) is a
# different run, not variance.
WALL_FACTOR = 3
WALL_GRACE_MS = 60000
TTFT_FACTOR = 3
TTFT_GRACE_MS = 10000
HARD_STOP_MARKERS = ("hard stop", "wall budget")

# A live model is allowed to lose scenario points; those checks are the score.
# Anything else in `failures` (a setup failure, a throw, a task that never
# finished) is an infrastructure failure and fails the release.
MODEL_EXCLUDED_SUITES = {"e2e", "repo-long"}
MODEL_QUALITY_FAILURES = (
    "oracle not matched",
    "final answer failed scenario checks",
    "prompt checks",
)


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
    paths = [path.strip() for path in paths if path.strip()]
    if not paths:
        return True  # an empty diff is ambiguous, not a docs-only tag
    for path in paths:
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
        if (
            isinstance(data, dict)
            and data.get("name")
            and data.get("prompt")
            and not data.get("hermetic_only")
            and data.get("suite") not in MODEL_EXCLUDED_SUITES
        ):
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
        failed = []
        for row in scenarios:
            if not isinstance(row, dict):
                continue
            reasons = row.get("failures") or []
            unexpected = [
                reason
                for reason in reasons
                if not str(reason).startswith(MODEL_QUALITY_FAILURES)
            ]
            if row.get("success") is False and not reasons:
                unexpected.append("scenario failed without a reason")
            if unexpected:
                failed.append(row.get("name", "?"))
        if failed:
            errors.append(
                f"{name}: {len(failed)} unexpected scenario failure(s): "
                f"{', '.join(failed[:5])}"
            )

    score = artifact.get("model_score")
    if not isinstance(score, (int, float)):
        errors.append(f"{name}: no numeric model_score")
    return errors, score


def discover_previous(results_dir):
    """The highest earlier release directory under .../releases, if any."""
    directory = Path(results_dir).resolve()
    if directory.parent.name != "releases":
        return None
    try:
        current = tuple(int(part) for part in directory.name.split("."))
    except ValueError:
        return None
    candidates = []
    for entry in directory.parent.iterdir():
        if not entry.is_dir() or entry == directory:
            continue
        try:
            version = tuple(int(part) for part in entry.name.split("."))
        except ValueError:
            continue
        if version < current and any(entry.glob("*.json")):
            candidates.append((version, str(entry)))
    return max(candidates)[1] if candidates else None


def previous_artifacts(previous_dir):
    """(model, profile) -> (filename, artifact) for a comparable release."""
    out = {}
    if not previous_dir:
        return out
    artifacts, _ = load_artifacts(previous_dir)
    for name, artifact in artifacts:
        score = artifact.get("model_score")
        if not isinstance(score, (int, float)):
            continue
        key = (artifact.get("model"), artifact.get("profile"))
        out[key] = (name, artifact)
    return out


def _median_tokens(row):
    """The median of the repeat prompt+completion totals, or None."""
    prompts = row.get("repeat_prompt_tokens")
    completions = row.get("repeat_completion_tokens")
    if (isinstance(prompts, list) and isinstance(completions, list)
            and prompts and len(prompts) == len(completions)):
        totals = []
        for prompt, completion in zip(prompts, completions):
            if not isinstance(prompt, (int, float)) or not isinstance(completion, (int, float)):
                return None
            totals.append(float(prompt) + float(completion))
        totals.sort()
        return totals[len(totals) // 2]
    prompt = row.get("prompt_tokens")
    completion = row.get("completion_tokens")
    if isinstance(prompt, (int, float)) and isinstance(completion, (int, float)):
        return float(prompt) + float(completion)
    return None


def _tool_calls(row):
    total = row.get("tool_calls_total")
    if isinstance(total, (int, float)):
        return float(total)
    calls = row.get("tool_calls")
    if isinstance(calls, list):
        return float(len(calls))
    return None


def behaviour_errors(name, artifact):
    """Compaction failures and hard stops are infrastructure, not model quality."""
    errors = []
    for row in artifact.get("scenarios", []):
        if not isinstance(row, dict):
            continue
        scenario = row.get("name", "?")
        if row.get("compaction_failures", 0):
            errors.append(f"{name}: {scenario}: {row['compaction_failures']} compaction failure(s)")
        for reason in row.get("failures") or []:
            text = str(reason).lower()
            if any(marker in text for marker in HARD_STOP_MARKERS):
                errors.append(f"{name}: {scenario}: hard stop: {reason}")
    return errors


def common_score_comparison(previous_name, previous, name, current, band):
    """Compare the mean per-scenario score of the scenarios in both artifacts.

    A release can add scenarios to the corpus. The top `model_score` is then a
    different weighted aggregate, so it is not a regression signal; the
    scenarios present in both artifacts are. The per-scenario score is 0..100;
    scaling the mean by 10 puts it on the 0..1000 model-score axis the band is
    expressed in.
    """
    errors = []
    previous_rows = {row.get("name"): row for row in previous.get("scenarios", [])
                     if isinstance(row, dict) and row.get("name")}
    current_rows = {row.get("name"): row for row in current.get("scenarios", [])
                    if isinstance(row, dict) and row.get("name")}
    common = sorted(set(previous_rows) & set(current_rows))
    if not common:
        return [f"{name}: no scenario present in both {previous_name} and {name}"], ""
    previous_scores = []
    current_scores = []
    for scenario in common:
        old = previous_rows[scenario].get("score")
        new = current_rows[scenario].get("score")
        if not isinstance(old, (int, float)) or not isinstance(new, (int, float)):
            return [f"{name}: {scenario}: missing score for the common-corpus band"], ""
        previous_scores.append(float(old))
        current_scores.append(float(new))
    previous_mean = sum(previous_scores) / len(previous_scores) * 10
    current_mean = sum(current_scores) / len(current_scores) * 10
    delta = current_mean - previous_mean
    if delta < -band:
        errors.append(
            f"{name}: common-corpus score {current_mean:.1f} is {-delta:.1f} below "
            f"{previous_name} ({previous_mean:.1f}), band {band:g}"
        )
    summary = f"common score {previous_mean:.1f}->{current_mean:.1f} ({delta:+.1f})"
    return errors, summary


def cost_comparison(previous_name, previous, name, current, cost_band, comparable_floor):
    """Compare the common successful corpus; return (errors, summary)."""
    errors = []
    previous_rows = {row.get("name"): row for row in previous.get("scenarios", [])
                     if isinstance(row, dict) and row.get("name")}
    current_rows = {row.get("name"): row for row in current.get("scenarios", [])
                    if isinstance(row, dict) and row.get("name")}
    common = sorted(name for name in previous_rows
                    if name in current_rows
                    and previous_rows[name].get("success") is True
                    and current_rows[name].get("success") is True)
    if not common:
        return [f"{name}: no scenario successful in both {previous_name} and {name}"], ""
    if comparable_floor and len(common) < comparable_floor:
        errors.append(
            f"{name}: only {len(common)} comparable successful scenario(s); "
            f"the cost floor is {comparable_floor}"
        )
    previous_tokens = current_tokens = 0.0
    previous_calls = current_calls = 0.0
    for scenario in common:
        old = previous_rows[scenario]
        new = current_rows[scenario]
        old_tokens = _median_tokens(old)
        new_tokens = _median_tokens(new)
        if old_tokens is None or new_tokens is None:
            errors.append(f"{name}: {scenario}: missing token data for the cost band")
            continue
        previous_tokens += old_tokens
        current_tokens += new_tokens
        old_calls = _tool_calls(old)
        new_calls = _tool_calls(new)
        if old_calls is None or new_calls is None:
            errors.append(f"{name}: {scenario}: missing tool-call data for the cost band")
            continue
        previous_calls += old_calls
        current_calls += new_calls
        old_wall = old.get("wall_ms")
        new_wall = new.get("wall_ms")
        if isinstance(old_wall, (int, float)) and isinstance(new_wall, (int, float)):
            ceiling = max(float(old_wall) * WALL_FACTOR, float(old_wall) + WALL_GRACE_MS)
            if new_wall > ceiling:
                errors.append(
                    f"{name}: {scenario}: wall {new_wall:.0f}ms exceeds the pathology "
                    f"ceiling {ceiling:.0f}ms ({previous_name} {old_wall:.0f}ms)"
                )
        old_ttft = old.get("ttft_ms")
        new_ttft = new.get("ttft_ms")
        if isinstance(old_ttft, (int, float)) and isinstance(new_ttft, (int, float)):
            ceiling = max(float(old_ttft) * TTFT_FACTOR, float(old_ttft) + TTFT_GRACE_MS)
            if new_ttft > ceiling:
                errors.append(
                    f"{name}: {scenario}: TTFT {new_ttft:.0f}ms exceeds the pathology "
                    f"ceiling {ceiling:.0f}ms ({previous_name} {old_ttft:.0f}ms)"
                )
    if previous_tokens <= 0:
        errors.append(f"{name}: no usable previous token data for the comparison")
    else:
        token_delta = (current_tokens - previous_tokens) / previous_tokens
        if token_delta > cost_band:
            errors.append(
                f"{name}: prompt+completion tokens up {token_delta * 100:.1f}% on the "
                f"common corpus (band +{cost_band * 100:.0f}%)"
            )
    if previous_calls <= 0:
        if current_calls > 0:
            errors.append(
                f"{name}: tool calls added where the previous common corpus had none"
            )
    else:
        call_delta = (current_calls - previous_calls) / previous_calls
        if call_delta > cost_band:
            errors.append(
                f"{name}: tool calls up {call_delta * 100:.1f}% on the common corpus "
                f"(band +{cost_band * 100:.0f}%)"
            )
    token_pct = (
        (current_tokens - previous_tokens) / previous_tokens * 100
        if previous_tokens else 0.0
    )
    call_pct = ((current_calls - previous_calls) / previous_calls * 100) if previous_calls else 0.0
    summary = (
        f"cost {len(common)} common: tokens {previous_tokens:.0f}->{current_tokens:.0f} "
        f"({token_pct:+.1f}%), calls {previous_calls:.0f}->{current_calls:.0f} "
        f"({call_pct:+.1f}%)"
    )
    return errors, summary


def check(results_dir, version, corpus_dir=None, previous_dir=None, band=DEFAULT_BAND,
          model=None, profile=None, min_scenarios=None, changed_paths=None,
          cost_band=DEFAULT_COST_BAND, comparable_floor=0):
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

    previous = previous_artifacts(previous_dir)
    report.say(
        f"bench release check {version} (score band {band:g}, "
        f"cost band +{cost_band * 100:.0f}%, scenarios >= {floor})"
    )
    for name, artifact in sorted(artifacts):
        problems, score = validate_artifact(
            name, artifact, version, floor, model, profile
        )
        for problem in problems:
            report.fail(f"bench: {problem}")
        for problem in behaviour_errors(name, artifact):
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
            prev_name, prev_artifact = previous[key]
            prev_score = float(prev_artifact.get("model_score", 0.0))
            previous_names = {row.get("name") for row in prev_artifact.get("scenarios", [])
                              if isinstance(row, dict)}
            current_names = {row.get("name") for row in artifact.get("scenarios", [])
                             if isinstance(row, dict)}
            if previous_names == current_names:
                drop = prev_score - float(score)
                if drop > band:
                    report.fail(
                        f"bench: {name}: score {score:.1f} is {drop:.1f} below "
                        f"{prev_name} ({prev_score:.1f}), band {band:g}"
                    )
                else:
                    line += f" (vs {prev_name}: {score - prev_score:+.1f})"
            else:
                # The corpus changed: the top score is a different weighted
                # aggregate. Compare the scenarios the two releases share.
                score_errors, score_summary = common_score_comparison(
                    prev_name, prev_artifact, name, artifact, band
                )
                for problem in score_errors:
                    report.fail(f"bench: {problem}")
                line += (
                    f" (vs {prev_name}: top score {score - prev_score:+.1f}; "
                    f"corpus changed, {score_summary})"
                )
            cost_errors, cost_summary = cost_comparison(
                prev_name, prev_artifact, name, artifact, cost_band, comparable_floor
            )
            for problem in cost_errors:
                report.fail(f"bench: {problem}")
            if cost_summary:
                line += f"; {cost_summary}"
        else:
            line += " (no previous artifact for this model/profile; cost bands not measured)"
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
    parser.add_argument("--cost-band", type=float, default=DEFAULT_COST_BAND,
                        help="fractional token/tool-call ceiling on the common corpus")
    parser.add_argument("--comparable-floor", type=int, default=DEFAULT_COMPARABLE_FLOOR,
                        help="minimum scenarios successful in both artifacts")
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
    previous_dir = args.previous_dir or discover_previous(results_dir)
    corpus_dir = args.corpus_dir or str(root / "bench" / "scenarios")
    report = check(
        results_dir,
        version,
        corpus_dir=corpus_dir,
        previous_dir=previous_dir,
        band=args.band,
        model=args.model,
        profile=args.profile,
        min_scenarios=args.min_scenarios,
        changed_paths=read_changed_paths(args.changed_paths_file),
        cost_band=args.cost_band,
        comparable_floor=args.comparable_floor,
    )
    for line in report.lines:
        print(line)
    return report.code


if __name__ == "__main__":
    sys.exit(main())
