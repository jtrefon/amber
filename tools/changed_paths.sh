#!/bin/sh
# Classify a changed-file list for CI path gating.
#
# Used by .github/workflows/ci.yml's `changes` job to decide which heavyweight jobs are
# worth running. Everything downstream depends on this being right in one direction only:
# a false "this is C++" costs some CI minutes, a false "this is not C++" silently skips
# every C++ gate on a PR that changed C++.
#
# So the question is asked as an ALLOWLIST of documentation, not a denylist of code. The
# old form asked "does anything match lib/|src/|tui/|...?" and answered no for anything it
# did not recognise -- a new top-level directory, or a typo in a path, and the whole C++
# suite was skipped on a PR that very much changed C++. An unrecognised path now means
# "assume code", which fails toward running the gates.
#
# Reads paths on stdin, writes "cpp=<bool>" and "web=<bool>" as shell assignments.
#
# `--self-test` runs the cases that matter and exits non-zero if any misbehaves. The
# build-hygiene gate runs it, because a classifier that silently stops classifying is
# indistinguishable from one that claims every PR is documentation.

set -eu

# Paths that cannot affect a compiled artefact or its gates. Anything not matched here
# is treated as C++-relevant on purpose.
is_docs_only_path() {
    case "$1" in
        docs/*|*/docs/*)          return 0 ;;
        website/*|*/website/*)     return 0 ;;
        LICENSE|LICENSE.*)         return 0 ;;
        CHANGELOG|CHANGELOG.*)     return 0 ;;
        CODE_OF_CONDUCT*)          return 0 ;;
        .gitignore|.gitattributes) return 0 ;;
        .git-blame-ignore-revs|.DS_Store) return 0 ;;
        # Human-facing templates and assistant config. These come before the *.md rule:
        # a template is Markdown at a nested path, and case matches in order, so the
        # generic rule below would shadow them and call them code.
        .github/ISSUE_TEMPLATE/*)  return 0 ;;
        .github/PULL_REQUEST_TEMPLATE*) return 0 ;;
        .claude/*|.cursor/*)        return 0 ;;
        # Root-level prose only. Scoped to the root so prompts/*.md stays
        # C++-relevant: editing a prompt changes agent behaviour at runtime, and
        # bench/scenarios/*.md are fixtures an oracle scores.
        *.md)                      case "$1" in */*) return 1 ;; *) return 0 ;; esac ;;
        *)                         return 1 ;;
    esac
}

is_web_path() {
    case "$1" in
        website/*) return 0 ;;
        *)         return 1 ;;
    esac
}

# The workflow's own definition decides both halves, since a change to it can move either.
is_workflow_path() {
    case "$1" in
        .github/workflows/ci.yml) return 0 ;;
        *)                        return 1 ;;
    esac
}

classify() {
    seen=0
    all_docs=true
    web=false
    both=false

    while IFS= read -r path; do
        [ -z "$path" ] && continue
        seen=$((seen + 1))
        if is_docs_only_path "$path"; then :; else all_docs=false; fi
        if is_web_path "$path"; then web=true; fi
        if is_workflow_path "$path"; then web=true; both=true; fi
    done

    # An empty range is an empty PR range -- a tag build, or a fetch that found nothing.
    # There is nothing to prove, so run everything rather than claim there was nothing
    # to check.
    if [ "$seen" -eq 0 ]; then
        echo "cpp=true"
        echo "website=true"
        return 0
    fi

    # The workflow definition is not itself documentation, so editing it is never a
    # docs-only diff even though every path in it lives under .github/.
    if [ "$all_docs" = true ] && [ "$both" = false ]; then
        echo "cpp=false"
    else
        echo "cpp=true"
    fi
    echo "website=$web"
}

self_test() {
    fails=0
    expect() {
        desc="$1"; want_cpp="$2"; want_web="$3"
        shift 3
        got="$(printf '%s\n' "$@" | classify)"
        got_cpp="$(printf '%s' "$got" | sed -n 's/^cpp=//p')"
        got_web="$(printf '%s' "$got" | sed -n 's/^website=//p')"
        if [ "$got_cpp" != "$want_cpp" ] || [ "$got_web" != "$want_web" ]; then
            echo "  FAIL $desc: want cpp=$want_cpp web=$want_web, got cpp=$got_cpp web=$got_web" >&2
            fails=$((fails + 1))
        fi
    }

    # Code changes must run the C++ suite.
    expect "lib change"          true  false lib/agent.cpp
    expect "tui header"          true  false tui/tui.h
    expect "test change"         true  false tests/run_tests.cpp
    expect "Makefile.in"         true  false Makefile.in
    expect "prompt change"       true  false prompts/system.md
    expect "scenario fixture"    true  false bench/scenarios/x/y.md
    expect "clang-format"        true  false .clang-format
    expect "completions"         true  false completions.json
    expect "workflow change"     true  true  .github/workflows/ci.yml

    # The fail-safe case: an unrecognised path must be treated as code.
    expect "unknown new dir"     true  false some_new_dir/thing.cpp
    expect "unknown root file"   true  false some_new_thing.txt
    expect "no extension"        true  false Makefile.am

    # Documentation-only diffs skip it.
    expect "docs only"           false false docs/architecture.md
    expect "nested docs only"    false false docs/spec/plugins/plugin-framework.md
    expect "root markdown"       false false AGENTS.md
    expect "website only"        false true  website/index.html
    expect "licence only"        false false LICENSE
    expect "issue template"      false false .github/ISSUE_TEMPLATE/bug.md
    expect "gitignore only"      false false .gitignore

    # One code file is enough to require the suite.
    expect "docs plus code"      true  false docs/a.md lib/agent.cpp
    expect "root md plus code"   true  false README.md lib/agent.cpp
    expect "website plus code"   true  true  website/a.html lib/agent.cpp

    # An empty range runs everything rather than claiming there was nothing to check.
    expect "empty diff"          true  true

    if [ "$fails" -ne 0 ]; then
        echo "changed_paths.sh: $fails self-test case(s) failed" >&2
        return 1
    fi
    echo "changed_paths.sh: self-test passed"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

classify