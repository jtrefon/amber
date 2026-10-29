# AGENTS.md, amber (cpp-agent)

C++17 AI agent harness: a core library (`libagent_core.a` + `libagent_tools.a`)
plus a headless CLI (`amber-cli`) and an ncurses TUI (`amber`, the flagship),
driven by an OpenAI-compatible LLM API.

## Build & verify

- `make` works from a fresh checkout: `GNUmakefile` auto-runs `./configure` to
  generate `Makefile` from `Makefile.in`. You rarely need to call `./configure`
  by hand.
- `make` builds everything (`lib cli tui`). The binaries and archive land in the
  repo root (`amber`, `amber-cli`, `libagent_core.a`, `libagent_tools.a`), in-tree, not in a `build/`.
- `make test` builds and runs the unit suite (`run_tests`). The binary is
  interrogable: `./run_tests --list` prints every test name, a positional
  argument is a substring filter (`./run_tests agent_loop`), and each test runs
  under a watchdog (`AMBER_TEST_TIMEOUT_MS`, default 120000) that names the
  hung test and exits non-zero instead of hanging the run. `make check` is a
  separate, lighter gate (`smoketest` + `tests/build_hygiene.sh` build
  invariants), do not confuse the two. It also runs
  `tools/obfuscation_guard.py`, which scans tracked files for the signature of
  an injected payload (obfuscator markers, or a single line of code longer
  than 500 chars): every other gate reads a build or a metric, none of them
  reads file contents, which is how an obfuscated dropper sat in `website/`
  for months with a green build. The CI job has no `if:` — it runs on every
  PR, including docs- and website-only ones.
- **Measuring coverage (there is no `make coverage`):** rebuild instrumented,
  `CXXFLAGS="-O0 -g --coverage" make`, then
  `gcovr --root . --xml-pretty -o coverage.xml` (the recipe the `coverage` job
  in `.github/workflows/ci.yml` runs). `tools/coverage_gate.py --report
  <path> --diff-base <ref>` reads that report and **fails closed** without it,
  so a coverage question asked without this build can only ever be answered
  "not measured" — say that, rather than re-checking the gate's own tests.
- **`w/` and `.wt/*` are separate git worktrees inside this tree**, not the
  project. They carry their own `run_tests`, `tools/`, and stale test results
  (e.g. `w/`'s `1105 passed, 9 failed`). The repo root is the only source of
  truth: build and test here, and never read a sibling worktree's artefacts as
  this project's state.
- `make lint` runs **clang-tidy** over every project source (third_party
  excluded) using the `.clang-tidy` config in the repo root. It is fast enough
  to gate changes on. `make analyze` runs **cppcheck** as an independent,
  cross-TU second opinion (slower; runs in parallel and skips the vendored
  nlohmann/json header). Both must come back clean before a commit.
- `make lint` is a **ratchet**, not a cliff: `tests/lint_baseline.json` records
  how many findings each (file, check) pair has, and the gate fails only when a
  count **grows**. That is what lets a check with existing findings be switched
  on at all — `readability-function-cognitive-complexity` is enabled that way
  (CX1), with 31 known findings. Fix findings and lower the counts with
  `make lint-baseline-update`; the file can only shrink. The counts are per
  (file, check) rather than per line, so edits that move code do not churn it.
  `tools/lint_baseline.py --report` prints the current counts.
- CI blocks on `make && make test` under **both** `g++` and `clang++`
  (`CXX=g++` / `CXX=clang++`). Both those jobs, and the macOS job, set
  `WERROR=1`, so a compiler or linker warning fails the build; the tree is at
  zero and stays there. Local builds keep the default (`WERROR=0`) so a
  developer's newer toolchain cannot block unrelated work: `make WERROR=1`
  opts in. `tests/build_hygiene.sh` **P14** asserts the knob still expands to
  `-Werror` and the platform's fatal-warnings flag, so it cannot silently
  measure nothing.
- `make lint` (clang-tidy) and `make analyze` (cppcheck) gate CI as separate
  compiler-agnostic jobs (single run each, independent of the compiler matrix).
  `.clang-tidy` also enables the `misc-unused-*` family (dead-code surface).
  cppcheck is version-sensitive: checks such as `uninitMemberVarNoCtor` only
  exist from 2.21, so an older cppcheck reports fewer findings and the gate is
  quietly weaker. `CPPCHECK_MIN_VERSION` (Makefile.in) is the pinned floor —
  `make analyze` fails closed below it — and CI installs exactly that version
  from conda-forge, so bump the two together. Its build dir
  (`/tmp/cppcheck_amber`) is kept between runs and cached in CI: only changed
  files are re-analyzed, which is the difference between ~7 min and ~40 s.
  `rm -rf /tmp/cppcheck_amber` for a from-scratch run.
- `make duplicates` runs the cross-file duplicate-block detector
  (`tools/duplicate_detector.py`); it is a gating CI job, and scans `plugins/` and
  `bench/` as well as `lib/ tools/ tui/ tests/ src/`. `make
  format-check-changed BASE=origin/main` is the incremental clang-format gate
  (changed files only); full-tree `make format-check` stays informational.
  clang-format is version-sensitive and the runner pins **18.1.3** (Ubuntu's
  `clang-format`), which disagrees with 23.x *and* with 18.1.8 on braced-init
  and line-break placement — so a locally "clean" tree can fail CI. Match the
  runner (`pip install clang-format==18.1.3`) before reformatting.
- `make nesting` is a **cliff** (`tools/nesting_gate.py`: control-flow nesting
  > 4 fails). It is separate from NLOC/CCN because a 20-line function can be
  unreadable at five levels of nesting while a 40-line straight-line function is
  fine. Both `nesting` and `complexity` share `_lizard_version_check`.
- `make gate-report` runs every gate and writes `artifacts/GATE_REPORT.md` plus
  `artifacts/gate-report.json` — one table with all verdicts, so "what did the
  gates say?" is one file instead of six job logs. `make check` produces it and
  CI uploads it as a build artefact (GitHub's equivalent of an ADO build
  artefact: attached to the run, listed on the run page, downloadable with
  `gh run download <run-id>`). It also appends the table to
  `$GITHUB_STEP_SUMMARY`, so the verdicts render **on the run page itself** — an
  artefact alone is easy to miss, since it sits in a panel at the bottom of the run
  below every job and has to be downloaded. The coverage job does the same with the
  line/function percentages. Only the table goes inline; the full gate output stays
  in the job log and the artefact. The report never gates: a failure there is exit 2
  — a reporting failure, distinct from a gate finding something.
- `make hygiene` is a **cliff** (`tools/hygiene_gate.py`): no `using namespace`
  in a header, no raw `new`/`delete` in owned source, and every header compiles on
  its own. All three measured clean first (0 / 1 acknowledged / 0 of 172 failing),
  so there are no baselines — only zero to stay at. A finding is suppressed by
  acknowledging it inline (`// hygiene-allow: private ctor`), matched **on that
  line**: a marker in a paragraph above would otherwise suppress whatever statement
  happened to follow. `--report` counts suppressions.
  Self-containment compiles each header with the exact flags the build uses, passed
  in by the Makefile — measuring with different flags turns failures into artefacts
  (it reported 9 broken `tui/` headers once, purely because `NCURSES_CFLAGS` was
  empty locally). `bench/scenarios/` is excluded: that is code the benchmark *agent*
  writes and an oracle scores.
- `make debt` is a **cliff** (`tools/debt_gate.py`): no TODO/FIXME/HACK/XXX in
  owned C++ source. The tree is already at zero, so it is the cheapest debt
  ratchet there is — nothing to burn down, just stay at zero. A marker is
  suppressed by acknowledging it inline (`// TODO: x  (debt-allow: #412)`), and
  `--report` counts suppressions so they cannot accumulate silently. Vendored
  code (`include/nlohmann/json.hpp` has 13 of its own) is excluded.
- `make complexity` is the ratcheted gate (`tools/complexity_gate.py`:
  CCN>15, NLOC>40 or PARAM>6 via lizard, fail-closed). It **is** in `ci-gate`:
  the CCN and NLOC axes have empty baselines, so a new over-limit function fails
  the build rather than joining a list. It was advisory until the baselines
  emptied — with per-file counts it conflicted on every merge, which is what
  made gating it unsafe. The PARAM axis is newer and therefore *ratcheted*, not
  a cliff (18 accepted functions): a new wide function fails, an existing one
  may not get wider. Every CI
  job carries a `timeout-minutes` and the shared `install-deps` action bounds
  apt (`DPkg::Lock::Timeout` + `timeout` + noninteractive), so a hung
  dependency install fails in minutes instead of sitting until GitHub's 6 h
  job ceiling. Jobs that need only `./configure`'s probes (complexity) pass
  `cpp_toolchain: 'false'` to skip clang/ncurses/pkg-config.
- **Fuzzing** (`make fuzz-run`) is the only gate that reaches input nobody wrote.
  Everything else -- the 1039-case suite, ASan/UBSan, TSan -- runs a fixed corpus, so a
  defect needing an input no test author imagined is unreachable by CI. Three targets,
  chosen because they are the parsers fed by bytes we do not control: `mcp_decode_line`
  (MCP JSON-RPC), `StreamDecoder::on_write` (SSE framing off the model endpoint) and
  `md::render` (Markdown off model output). Built with `-fsanitize=fuzzer-no-link` so
  libFuzzer is guided, under ASan+UBSan, with the whole Markdown path instrumented
  rather than just its entry function. `FUZZ_CC` needs a clang with libFuzzer: Apple's
  ships none, and `make fuzz` **fails closed** there rather than reporting a pass it could
  not have performed. Not part of `all`; the `fuzz` CI job gates in the `cpp` dimension.
  Seeds live in `fuzz/corpus/`; libFuzzer accumulates into a gitignored `.fuzz-build/`
  so a run does not dirty the tree with thousands of inputs. One consequence worth
  knowing: a seed *must* stay invalid UTF-8, and `git diff` then emits undecodable
  bytes, which crashed `coverage_gate.py` with `UnicodeDecodeError`. Git output is
  bytes, so both reads decode leniently now.
- CI also runs an ASan+UBSan `make test` job, a **ThreadSanitizer** `make test`
  job (data races: the UI thread, per-window agent workers, the tool-dispatch
  hop and the detached catalog/plugin workers all share state, and ASan+UBSan
  cannot see races), a gcov coverage job uploading to
  Codecov (needs the `CODECOV_TOKEN` secret), and a separate CodeQL
  workflow (`security-and-quality` queries). The `changes` job gates every
  heavyweight job on which paths a PR touched — docs-only PRs skip the C++
  suite entirely. Branch protection requires only the `ci-gate` job, which
  aggregates the gating jobs and passes when they succeed or are legitimately
  skipped — a skipped job never reports a check run, so requiring the matrix
  jobs directly would leave meta-only PRs blocked on "Expected" forever.
- **`main` is locked, and it is worth knowing what that means before you try.**
  Branch protection on `main` requires a **pull request** (`required_approving_
  review_count: 0` — a PR is required, no approval needed), the `ci-gate` check
  with `strict` (so a green run on an older commit does not count), and it
  **applies to admins** (`enforce_admins: true`). Force-pushes, deletions and
  non-linear history are blocked, and PR conversations must be resolved.

  `enforce_admins` is the part that bit: with it off, protection does not apply
  to an admin, so a direct `git push origin main` sails past the required status
  check — required checks only gate *pull request merges*. Three consecutive
  red main commits got in that way before it was noticed. Verified by attempting
  the push and watching it be declined.

  So: **no direct pushes to `main`, including from an admin.** Feature branch,
  PR, wait for `ci-gate`. The escape hatch, if a gate is broken and an emergency
  push is genuinely needed, is
  `gh api -X DELETE repos/jtrefon/amber/branches/main/protection` — re-apply it
  afterwards, because a repo with protection removed will not notice.

  **A PR is armed, and stale PRs are refreshed by the train.** Auto-merge is on
  (`allow_auto_merge`), so `gh pr merge <n> --squash --auto` merges the moment
  the gates pass — no waiting on a terminal. `strict` checks still block a green
  PR once `main` has moved, and **GitHub's native merge queue cannot be enabled
  here**: the rulesets API rejects a `merge_queue` rule and no REST or GraphQL
  field turns it on. What stands in for it is the **labelled merge train**
  (`.github/workflows/merge-train.yml`, selection in `tools/merge_train.py`):
  when a CI run completes it lands at most one `CLEAN` PR carrying the
  `merge-train` label and updates the branches of the other labelled ones, so
  their gates re-run against the new `main` instead of waiting for somebody to
  press "Update branch". The update is a `GITHUB_TOKEN` push, which starts no
  workflow — so the train dispatches `ci.yml` on each refreshed branch, and
  re-arms any labelled PR whose head has zero check runs (a refresh made by an
  older train version). `workflow_dispatch` is therefore a first-class event in
  `gate_needs_check.py`, held to the push job set; without that the refreshed
  head has no `ci-gate` and the PR stays `BLOCKED` forever, which is exactly what
  happened on 2026-10-27. The trigger is `workflow_run` completion, not a timer:
  the schedule is only a backstop: its runs arrive hours late, so nothing
  that needs to happen promptly may depend on them. It never touches an
  unlabelled PR, a draft, a `DIRTY` one, or one whose gates have not passed — a
  train cannot resolve a conflict, and merging past a red gate is the failure the
  gates exist to prevent. With feature-batched delivery there are only a handful
  of feature PRs in flight, so the train is a landing mechanism, not a queue for
  task PRs. After a merge it dispatches `ci.yml` on `main` itself:
  a merge made with `GITHUB_TOKEN` creates no push event, so without that the run
  that validates `main` (the full suite, and `lint-full`, which only runs there)
  would silently never start.

  `ci.yml` still triggers on `merge_group`, so enabling the native queue later
  needs no workflow change; `tools/gate_needs_check.py` holds that event to
  every job that applies there (only the `pull_request`-only jobs may skip), and
  its self-test covers both the legitimate skips and the bypasses. A skip policy
  that treated an unknown event as "not applicable" would let a merge-group run
  skip every gate and still pass, which is why the event is listed explicitly.
- `make clean` removes in-tree `.o`/`.d`/binaries; `make distclean` also drops
  the generated `Makefile`.

## Gate integrity (why the gates can be believed)

The gates are the only thing protecting everything else, and until
`make gates-test` existed they had **no tests at all** — which is exactly how
the fail-open behaviour below survived. `tests/gates/` holds stdlib `unittest`
suites (no dependency install, so it runs in the `check` job via `make check`)
for `complexity_gate.py`, `class_size_gate.py`, `lint_baseline.py`,
`nesting_gate.py`, `debt_gate.py`, `coverage_gate.py`, `gate_report.py` and the
shared `cpp_source.py` lexer. When you
change a gate, change its tests in the same commit.

**A gate that measures nothing reports green.** Every gate here fails closed on a
degraded measurement, not just on a missing tool:

| failure mode | how it is caught |
|---|---|
| analyzer not installed / exits non-zero | version floor + explicit error (exit 2) |
| analyzer output format changed | `parse_lizard_output` counts lines it cannot parse; a non-zero count is an error |
| scan covered almost nothing | `MIN_FUNCTIONS_SANE` / `MIN_TYPES_SANE` floors, and a `--expect-tu` count for `lint` |
| a translation unit does not compile | `[clang-diagnostic-error]` is never counted as a finding, and fails the lint gate; `lint-baseline-update` refuses to record it |
| a brace-aware scanner lost sync | an unterminated type is reported as unscannable, not skipped |
| a function's extent cannot be closed | `nesting` reports it unmeasurable rather than scoring it zero |

Two more found by **mutation testing the gates themselves** — deliberately breaking
each one and confirming it goes red. Everything in the table above is checked at
`make check` or on CI, but a gate can be wired up correctly and still measure nothing:

- **A skip is not a pass** — but a skip has *two* independent legitimate causes, and
  conflating them is how this gate broke main. `ci-gate` used to accept `skipped` for
  every gating job, so `if: false` on a gate — or a path filter that misclassifying a
  directory — turned the pipeline green with that gate never having run. Closing that by
  judging skips against the single `cpp` flag was wrong in the other direction: `lint`
  and `dependency-review` are `pull_request`-only **by design**, so every push to `main`
  skipped them legitimately and ci-gate failed. Three consecutive red main runs went
  unnoticed because ci-gate is usually watched on pull requests.
  A skip is therefore acceptable only when the diff cannot affect that job's
  **dimension** (`cpp` / `web` / `always`) *or* the job does not apply to this
  **event**. The decision lives in `tools/gate_needs_check.py` — 61 cases run as
  `build-hygiene` **P10** — because the first three versions of it were written inline
  in YAML, where none could be tested locally and every one of them was wrong. An
  unknown GitHub result counts as a failure, and a job absent from its dimension table
  defaults to `always`, so forgetting to list one cannot make its skip acceptable.
- **The path filter is an allowlist.** `tools/changed_paths.sh` answers "is this
  documentation?" and treats *any unrecognised path as code*, because the costs are
  asymmetric: a wrong "yes" costs CI minutes, a wrong "no" skips every C++ gate on a
  PR that changed C++. The old denylist (`lib/|src/|tui/|…`) answered "no" to anything
  it did not recognise, including a new top-level directory. Its 22 cases run as
  `build-hygiene` **P9** — which caught a bug in the allowlist itself on its first run,
  a generic `*.md` rule shadowing `.github/ISSUE_TEMPLATE/`. The script emits
  `cpp=` and `website=`, which must match the `changes` job's declared outputs exactly:
  they were `web=` and `website=`, so `changes.outputs.website` was **always empty**,
  `website-build` and `website-smoke` had never once run, and nothing noticed because a
  skip counted as a pass.
- **`format-check-changed` measured committed work only.** `git diff BASE...HEAD` sees
  nothing uncommitted, so it printed `no C++ changes, skipping` and exited 0 on the
  files you were editing. It now unions the merge-base, staged and unstaged diffs. The
  fix lives in **`Makefile.in`, never `Makefile`** — `Makefile` is generated by
  `./configure` and gitignored, so an edit made there is silently lost on the next
  configure. That happened once: the fix shipped in a PR description while never
  reaching the repository.
- **Secret scanning** is the `gitleaks` action, a gating job in the `always`
  dimension. Nothing else here looked for a committed credential:
  `obfuscation-guard` reads tracked files for a payload signature, and CodeQL's
  C++ queries do not look for secrets. gitleaks rather than a hand-rolled regex
  scanner, because a scanner written here would be weaker than the maintained
  ruleset and would risk being a gate that looks like a check and measures
  nothing. `.gitleaks.toml` allowlists `tests/` (fixtures use realistic fake
  credentials on purpose, and they must keep looking like credentials for the
  redaction tests to test anything), `fuzz/corpus/` (arbitrary bytes), the npm
  integrity hashes, and vendored code.
- **Vendored third-party code is pinned by hash, and checked.** `md4c` was
  fetched from upstream `master`, so no revision was ever recorded — a known gap,
  not something to paper over with a plausible SHA. `build-hygiene` **P11**
  compares the vendored files against the sha256 table in
  `third_party/md4c/README.md` *and* requires the README to document it, so
  neither a silent edit to the vendored parser nor an undocumented update passes.
  Dependabot cannot see vendored code, which is the whole reason the table exists.
- **`duplicates` covers `plugins/` and `bench/`** (`bench/scenarios/` excluded via
  `--exclude`, for the same reason complexity and nesting exclude it).

Two consequences worth remembering:

- **`lint` emits a `lint-tidy-tu:<path>` sentinel per translation unit** and the
  gate is told how many to expect (`--expect-tu`). Zero findings is a legitimate
  result and cannot be told apart from a crashed run by counting; the sentinel
  count can. Without it the gate is green having measured nothing.
- **clang-tidy's exit status is meaningless** — it exits non-zero for ordinary
  findings, and `complexity` likewise ignores lizard's status because `-C 1 -L 1`
  makes every function a "warning" (`if 0 <= number < warning_count: return 1`).
  Success is judged from output, never from `$?`.

**Analyzer versions are pinned, because each is load-bearing.** `make analyze`
already pinned cppcheck and `format-check` pins clang-format 18.1.3; the same
reasoning now applies to the rest:

| tool | pin | why |
|---|---|---|
| cppcheck | 2.22.0, `CPPCHECK_MIN_VERSION` floor | older versions lack checks like `uninitMemberVarNoCtor`, so the gate is quietly weaker |
| clang-tidy | `clang-tidy-18`, `CLANG_TIDY_MIN_VERSION` floor | `.clang-tidy` enables whole families (`bugprone-*`, `readability-*`, …), so every check a new release adds is on; with an **empty** baseline that turns every PR red. Measured: 23 reports `bugprone-command-processor`, `readability-trailing-comma`, `readability-redundant-nested-if` and `readability-redundant-qualified-alias` that 18 does not |
| lizard | `lizard==1.24.0`, `LIZARD_MIN_VERSION` floor | the gate parses lizard's `-w` output with a regex, and its NLOC/CCN arithmetic has moved between releases — an older lizard makes every recorded size a lie |
| gcovr | `gcovr==8.6` | its report layout feeds `.codecov.yml` |
| clang-format | runner-provided (18.1.3) | 18.1.8 and 23.x disagree on braced-init and line breaks |
| gitleaks | `GITLEAKS_VERSION` pinned (8.30.1) | ships rules updates frequently, so the gate changes underneath you; the action also runs Node 20, which GitHub's runner stopped defaulting to in June 2026 |

## Gaps in the current gate set (measured, not assumed)

Known and deliberate, so nobody re-derives them:

- **Coverage is enforced twice, in-repo, and Codecov is not load-bearing.**
  Measured: the Codecov upload has been failing on every run with
  `Upload queued for processing failed: {"message":"Repository not found"}` (the
  Codecov GitHub App is not installed), hidden behind `fail_ci_if_error: false`,
  so **no Codecov status check is ever created** and the `patch: target: 80%` in
  `.codecov.yml` has never actually been evaluated. Rather than depend on it:
  - the **project** floor stays `gcovr --fail-under-line 80` over `lib/`,
    `tools/`, `plugins/` (measured 84.3% lines / 91.2% functions);
  - the **patch** rule the docs actually state ("new code paths must have ≥80%
    line coverage") is `tools/coverage_gate.py`, which reads the Cobertura report
    and the diff and fails below 80% of *added* lines. It fails closed: no
    report, no diff, or a report with no line data is an error, never a pass.

  `fuzz/` is declared not-measured alongside `tests/`: the harnesses are test
  drivers, built by a separate libFuzzer invocation under sanitizers.

  Two consequences worth knowing. Only instrumented extensions count, so editing
  `ci.yml`, a prompt or Markdown cannot fail the gate — a gate that fails on
  documentation gets switched off. And gcovr's `--json-summary` cannot be used
  here: its entries have **no `lines` key**, so the gate reads Cobertura XML
  (`--xml-pretty`), which the job already produced. The Codecov upload is kept
  for the dashboard but is `continue-on-error` and must not be mistaken for a
  gate. To see patch coverage without blocking, set the repository variable
  `PATCH_COVERAGE_ENFORCE=false`.

  **Expect the first real C++ PR to trip it.** Every PR since the gate landed added
  no C++, so it has only ever reported "no changed executable lines". Two things
  to know: a changed line the report omits is *not executable* (gcovr drops comment,
  blank and brace lines, so demanding coverage of them is wrong — a bug this gate
  shipped with and caught on its own first run), while a changed `.cpp` absent from
  the report *entirely* was compiled but not measured, and that fails closed. And
  `tui/` is instrumented but only exercised by the pty/e2e harness, so new TUI code
  with no harness coverage will read 0% — which is the rule working, not a bug.
- **Nesting is gated, but not by lizard.** `make nesting` measures it from the
  source with `tools/cpp_source.py` instead, because lizard's `ND` field is
  `max_nesting_depth` and is **0 for all 2422 C++ functions** — verified against
  a deliberately 6-deep function. Gating on lizard's number would have been a
  permanently green gate that measures nothing, which is the exact failure mode
  the section above exists to prevent. Cap 4, empty baseline (0 violations, 4
  functions *at* the cap, 39 at depth >= 3).
- **`make lint` cannot be reproduced locally.** clang-tidy is not installed on a
  default macOS toolchain, and the two reachable builds disagree: CI installs
  `clang-tidy-18` (`1:18.1.3-1ubuntu1`) from apt, while the only pip wheel is
  `18.1.8`, and they did not agree on `bugprone-unchecked-optional-access` -- CI
  reported a finding the pip build did not. So "run the gates before pushing"
  only covers half of them locally, and a clean local `lint-changed` is not
  evidence about a finding. Matching the runner needs an LLVM 18 toolchain
  (`brew install llvm@18` and `CLANG_TIDY=/opt/homebrew/opt/llvm@18/bin/clang-tidy`),
  which is not present here.
- **Branching needs no separate axis**: CCN is the standard measure of decision
  points, and a branch count would be the same number twice.
- **`dependabot`: `http-cache-semantics` is open with no patched version.** The
  advisory is *high* -- "max-stale handling can disclose cross-user cached
  responses" -- and there is no fix to take, so it cannot be closed by an upgrade
  or an `overrides` pin. Measured rather than waved through: it is a transitive
  dependency of `astro` in `website/package-lock.json`, not dev-only, and the
  disclosure requires a shared HTTP cache in front of the server. The website is a
  static docs site built by Astro; the affected code path is the dev/preview
  server, which is not exposed to multiple users in CI or in normal use. Recorded
  here so the alert is not re-triaged from scratch each week, and revisited if the
  website ever gains a served-with-cache path.
- **No file-length cap.** Class size is gated (max type is 169/200), but total
  file length is not, so a file can grow by spreading across translation units.
- **The CCN axis is finished; the NLOC axis is not worth finishing.** CCN
  `15 -> 14` was tightened once the four functions sitting at 15 were cleared,
  and nothing is at or over 14 now, so it is a cliff on an empty baseline --
  which is what a cap is supposed to be. It stops there: measured CCN has
  median 3, p90 8, p99 12, max 13, so a cap anywhere near 13 says nothing true
  about the code, and the remaining 16 functions at CCN >= 13 are ordinary.
  The axis is exhausted; further burn-down there is theatre.

  NLOC is different, and the distinction matters. `NLOC_MAX` is still 40 with
  30 functions at >= 37 -- but **13 of those 30 have CCN <= 8**: flat, long,
  linear code (`loop_probe_fail_streak` is 39 lines at CCN 5). Reaching
  `NLOC_MAX = 36` means 41 refactors, most of them cosmetic, optimising a
  measure of *length* that has stopped tracking difficulty. The NLOC burn-down
  is deliberately stopped for that reason.

## Compilation gotchas

- Header dependency files (`.d`, via `-MMD -MP`) are generated, not committed
  (`*.d` is gitignored). If you change a struct layout or any header, rebuild,
  stale `.o` from missing `.d` entries silently causes ABI/heap-corruption bugs
  at runtime (called out in the Makefile). When in doubt, `make clean && make`.
- `include/agent/version.h` is **generated** by `./configure` from
  `version.h.in`; do not hand-edit it, and don't commit a stale one.
- On macOS, `./configure` needs Homebrew ncurses on `PKG_CONFIG_PATH` or the
  TUI silently falls back to the SDK's non-wide ncurses and `mvaddnwstr`
  fails to compile. `GNUmakefile` sets this automatically when brew has ncurses,
  so a plain `make` works from a fresh checkout; a *manual* `./configure` still
  needs
  `PKG_CONFIG_PATH=/opt/homebrew/opt/ncurses/lib/pkgconfig ./configure`
  (check the emitted `NCURSES_CFLAGS` line mentions `ncursesw` + `-DNCURSES_WIDECHAR`).
- `compile_flags.txt` (for clangd/editors) is minimal; the real include paths
  (`-Iinclude -Isrc -Itools -I.`) and flags come from the Makefile/configure.

## Architecture boundaries

- `lib/` + `include/agent/` is the UI-free core: LLM client, tool registry,
  agent loop, prompt/markdown loader, built-in tools. Keep UI concerns out.
- `src/amber-cli` is the headless CLI; `tui/` is the ncurses client (`amber`).
  Both only *link* `libagent_core.a` + `libagent_tools.a` and communicate via
  `AgentHooks`. `tui/` must never be depended on by `lib/`.
- `bench/` is the benchmark & KPI harness (`amber-bench`): scenario loader,
  oracle scorer, recorder (an `AgentHooks` observer), KPI aggregation, static
  template engine. Same layering rules as the clients, `bench/` only links
  the libraries, never touches the engine. Hermetic mode (fake LLM) is
  deterministic and safe for CI; live mode targets any OpenAI-compatible
  endpoint. Specs: `docs/spec/benchmark/`.
- Tools live in `tools/` (read/write/search/bash). The search tool is
  pluggable: `mode="grep"` (default, wraps `grep -rnI`) or `mode="semantic"`
  (dependency-free lexical index). Swap only `embed()` to use a real model.
- System/tool prompts are Markdown in `prompts/` (`system.md`, `tools.md`),
  loaded at runtime, editing those changes agent behavior without recompiling.

## Plugin architecture

The plugin system is the harness extensibility backbone: plugins contribute
tools, LLM providers, prompt blocks, status segments, panels, wallets and
allowances, and observe the agent loop through typed events. Commands,
per-plugin settings and log sinks are **not** extension points (removed or
deferred, see the developer guide's availability table).

**Binding model (see the spec before touching any of it):**

- **Three mechanisms.** Contribution registries (things that *exist*), typed
  events (things that *happen*), host services (things the plugin needs the host
  to do). Registration never happens through events; UI state is pulled at render
  time, not pushed from worker threads.
- **Declare, don't install.** A core plugin declares typed `Capability` objects;
  the runtime installs each into its registry and records it in a per-plugin
  ledger. `disable()` unwinds the ledger in reverse order, if a contribution can
  survive deactivation, the ledger is broken and that is a tested invariant.
- **Typed events over the tested `EventBus`.** Payload structs in
  `include/agent/events.h`; no `void*` in the plugin-facing API. Unsubscribed
  `publish()` is a single atomic load, **no per-token events, ever**
  (streaming stays an `AgentHooks`/UI concern).
- **Two tiers.** Core plugins are compiled in (`plugins/<id>/`, registered in
  `make_bundled_plugins()`); external plugins are separate processes
  (`tools/plugins/`, JSON-RPC over stdio) and contribute tools only. Runtime
  loading (`dlopen`) is deferred but the registries and state layout
  (`~/.config/amber/plugins/<id>/plugin.conf`) are shaped for it.
- **Provider plugins are the flagship consumer, and the conversion is done.**
  Every provider amber ships (`plugins/custom|openrouter|kilocode|anthropic|gemini|…`)
  is a plugin, and the core declares **none**: with no plugins registered,
  `/provider list` is empty. A provider capability is flavor + optional dialect
  factory + presets; wire behavior lives in the `Dialect`
  (`docs/spec/llm-client/dialect.md`), a provider plugin must not open its own
  HTTP client, and `lib/dialect.cpp` registers only the transport's own `openai`
  protocol so a disabled provider plugin takes its protocol with it. The one
  provider name left in core is `Config::provider_name`'s default, a default
  *selection*, not a definition.
- **Status:** the framework runs. Typed events fire at the agent and tool sites,
  capabilities install into typed registries through the ledger, `/get plugin`
  and `/set plugin on|off <id>` control state persisted in
  `~/.config/amber/plugins/<id>/plugin.conf`, the status bar is composed from a
  registry, and both hosts (`src/main.cpp`, `tui/tui_main.cpp`) construct a
  `PluginRuntime`. The runtime lives in `lib/plugin_runtime.cpp`; the bundled set
  in `lib/plugins_bundled.cpp`. The target is a microkernel, amber as
  orchestrator plus plugin registry, everything else arriving as a plugin, so no
  change may add core domain state that a later extraction would have to unpick.
- Spec: `docs/spec/plugins/plugin-framework.md` (design record, decisions,
  scenarios).
- Tracker: `docs/plugin-framework-tracker.md` (phases PF-1..PF-6, decision log,
  deferred register with reasons).
- Contributor guide: `docs/spec/plugins/developer-guide.md` (its availability
  table is the single status surface).

## Conventions

- Style: `.clang-format` (LLVM-based, 4-space, no tabs, 100 cols). Run
  `clang-format -i <files>` on touched code. No comments that restate code.
- New source files need no copyright/SPDX header, keep the first line functional.
- Commits: imperative mood, scoped prefixes (e.g. `tui: fix drawer scroll`).
  Tests for behavior changes go in `tests/run_tests.cpp`.

## Security model (treat LLM output as untrusted)

- read/write confine paths to the workspace root (default cwd, override with
  `AMBER_WORKSPACE`); absolute paths and `../` escapes are rejected.
- bash tool is approval-gated and fail-safe (denied if no approver). CLI prompts
  on a TTY, denies when stdin is not a TTY unless `--yes`. TUI shows a dialog.
  Default timeout 60s, output capped 64 KiB.
- Keep the agent unprivileged (container / dedicated dir).

## Command tree architecture (JSON-driven, zero hardcoded completion)

`completions.json` is the **single source of truth** for slash-command
structure: namespace branches (unlimited nesting), short help (drawer),
man pages (`?` popup), and leaf `action`s (internal command mapping, what
the branch executes). Dispatch (`handle_slash`), completion
(`update_completions`), and the drawer (`draw_drawer`) all derive from the
tree in `SettingRegistry`; C++ handlers are pure `(action, arg)` closures.

- **Schema**: every node is `{help, man, action?, children{...}}`; the last
  leaf of a branch carries the `action` (e.g. `core.config.get.model.list`).
- **Namespaces are keyed by their full display path** (`get.model` ≠
  `set.model`); dotted `/get` lookups resolve exact → `get.<key>` →
  `set.<key>` (`resolve_key` in setting_registry.cpp).
- **Dynamic values are feed leaves**: runtime state and external
  integrations merge leaf subtrees via `merge_completions_json` (deep merge,
  static fields preserved, children unioned; MCP/plugin/feeds never clobber
  documented nodes). Each leaf carries a **generated action**
  (`<parent action>.<leaf key>`) and the feed registers the handler closure.
  Existing feeds: `refresh_model_list` (set.model), `refresh_policy_feed`
  (get/set policy rule, the permission system, unchanged, surfaced in the
  tree), `refresh_job_feed` (job kill/read), `refresh_plugin_feed`
  (get.plugin.info and set.plugin.on|off — ids hang under their verb so the
  drawers stay command lists however many plugins register), plus
  `mcp_completion_subtree`. New dynamic content = a new feed, never a C++
  completion lambda.
- `SettingRegistry::complete(ns)` returns the **direct children** of a
  namespace (tree-walked) so drawer rows and completions stay 1:1 for Enter
  dispatch. The legacy flat `palette::Command` carries display metadata only
  (name/aliases/usage/help); there is no `complete_arg`/`current_value`.
- **Two host-owned alias tiers in `completions.json`.** The `aliases` map holds
  the *visible* root spellings (`model`, `compress`, `clear`, `stop`, `save`,
  `sessions`, `mcp`, …): they complete and appear as drawer rows pointing at
  the domains they dispatch into (`(→ /set model)` hint), resolve/dispatch
  through the canonical path so help, man and actions are the target's own,
  and `resolve_key` probes them after the exact index and before the
  `get.`/`set.` fallbacks (`context`, `filter` and `help_for` follow them).
  The `legacy` map holds the *hidden* spellings: dispatch-only, never drawer
  or completion rows. Neither map accepts a key shadowing a documented root or
  dual-booked across maps, refusals count in `alias_collisions()`, and a feed
  or plugin merge naming either map is refused — only `completions.json`
  authors alias material.

### Hard rule: the JSON mapper is the only command surface (no exceptions)

`completions.json` is the single source of truth for every slash command, its
children, help/man, aliases, choices, ranges and action. The drawer, completion,
`?` help page and `handle_slash` are views of that tree. Code supplies
behaviour, never shape:

- **No code-side command surface.** No hardcoded command paths in handlers
  (`handle_slash` walks the tree and dispatches the deepest documented node's
  `action`; handlers are pure `(action, arg)` closures). No C++ completion,
  choice or usage lists. No `Setting` metadata in code: `Setting` is a dotted
  key plus getter/setter, and help/placeholder/type/range live in the mapper.
  A missing command is a missing JSON node, never an `if (arg.rfind(...))`.
- **No patching the tree in code.** `SettingRegistry` is the only writer;
  `load_completions_json` the only loader and `merge_completions_json` the only
  merge. Do not write `tree_` directly, rename nodes, or overlay fields after
  load.
- **Documented fields are immutable.** A merge may append children (a
  documented command may also be a namespace: `/set model` is the switch and
  the model list) and may refresh fields on nodes it introduced, but it never
  rewrites an `action`/`help`/`man`/`aliases`/`choices`/`range` that came from
  `completions.json`. Collisions are counted (`merge_collisions()`); the
  documented value wins. The index is rebuilt from the merged tree, so a
  blocked overwrite cannot leak into help or choices.
- **Dynamic namespaces are JSON subtrees, not code lists.** Feeds
  (`refresh_*_feed()`, `mcp_completion_subtree()`) build a
  completions.json-shaped subtree and merge it; they register only the leaf
  action closures (`register_action`).
- **Plugins contribute a JSON subtree, never a code-shaped patch.** External
  plugins supply `completion` in `manifest.json`; bundled plugins supply
  `CommandSpec::subtree`. A contribution merges additively like a feed: it may
  extend a documented namespace (the MCP plugin contributes `/get mcp`) and add
  children, but it never rewrites a documented `action`/`help`/`man` and it
  cannot name a core action — every action is host-derived as
  `plugin.<id>.<path>`. A field collision is counted (`merge_collisions()`);
  the documented value wins.
- **No dead legacy dispatch.** When a feed/tree supersedes a hand-written
  branch, delete the branch; a namespace's bare usage page is allowed only
  when its action is registered.

The command surface is `completions.json` + JSON-subtree feeds; anything a user
can type must be resolvable there. Full contract:
`docs/spec/tui/command-tree.md`.

## Context stack architecture (immutable, hash-chained)

The `Context` class (`include/agent/context.h`) is a **pure stack**: messages are
sealed on `push()` and can never be modified in-place. The only mutation
operations are:

| Operation | What it does |
|-----------|-------------|
| `push(msg)` | Append a sealed message to the **top** of the stack. |
| `pop()` | Remove the **most recently pushed** message (LIFO). |
| `clear()` | Remove all messages. Used by the compression rebuild after assembly. |
| `get_all()` | **Read-only** view of the entire stack. Asserts FNV-1a hash-chain integrity before returning. |

Every `push()` computes `h_i = FNV(prev_hash || msg)` and stores it in a parallel
deque. `pop()` restores the previous hash in O(1). `get_all()` recomputes the
entire chain from the stored messages, any in-place mutation (`const_cast`,
rogue `replace` method, direct deque access) breaks a link and crashes with
`assert` in debug builds.

**Rules:**
- NEVER add a mutation method (replace, insert, update, set_message, etc.).
  If you need to rebuild the context, call `clear()` then `push()` each message.
- NEVER modify a message after it has been pushed (including via `const_cast`).
- NEVER add a mutex inside `Context` or change `get_all()` to return a copy.
  Thread safety is by **single ownership**, not locking: exactly one thread
  (the agent/compress worker) mutates a given `Context`; every other consumer
  interacts only via immutable snapshots taken when the owner is quiescent, or
  via `ContextEventSource` events.
- The `assert(verify_chain())` in `get_all()` is the integrity gate. If you
  bypass `get_all()` to read the deque directly, you are responsible for
  verifying the chain yourself.
- See `tests/run_tests.cpp` (`context_hash_chain_integrity`) for the test that
  exercises every mutation path and verifies the chain survives.
- The compression pipeline is **pure**: it reads the context into a working
  copy and never pushes/pops the live deque. KV reuse between the classify and
  extract LLM calls comes from content-identical prefixes (the extract request
  replays the classify request), not from mutating the live context. The
  rebuild (`clear()` + `push()`) happens only on success, in `run_compression`.
- Full design: `docs/spec/context/context-ownership-and-parallel-compression.md`.

## Runtime / config

- `amber.conf` sets `api_base`/`model`/`system_prompt`/`tools_prompt`. Defaults
  point at a local OpenAI-compatible endpoint (`localhost:8081/v1`).
- Streaming via SSE; disable with `--no-stream` or `AMBER_STREAM=0`.
- Releases are tag-driven (`vX.Y.Z`; tags with `-` are pre-releases), see
  `.github/workflows/release.yml`.

### Where amber keeps its own files

`agent::config_paths()` (`lib/config_paths.cpp`) is the single source of truth
for the layout; `/get config paths` prints it and the bundled `amber-config`
skill renders its body from it, so the two cannot disagree. The shape, per the
XDG base directories: global config in `$XDG_CONFIG_HOME/amber` (providers,
skills, MCP, plugin state), **shareable** project config in
`<project>/.amber` (skills, MCP, plugins -- created only when something is
added), **private per-project state** in
`$XDG_STATE_HOME/amber/projects/<slug>` (sessions, settings, logs, experience,
policy), and regenerable catalogues in `$XDG_CACHE_HOME/amber`. `$HOME` is not
special-cased: running amber there creates no project dir. Two consequences for
tests and debugging:

- Anything that touches a path must redirect the roots: `XdgGuard` in
  `tests/run_tests.cpp` sets config/state/cache, and the pty fixture sets
  `XDG_STATE_HOME`/`XDG_CACHE_HOME` for its child. A test that forgets one
  reads the developer's real state.
- The path resolvers are pure (asking creates nothing); callers create what
  they write, and `Workspace::adopt_legacy_state()` copies a pre-move artefact
  into the state dir once, never deleting the original.

### The local inference service is INVOLATE, never touch it

The OpenAI-compatible endpoint on `:8081` is served by the **`llama-turboq`
systemd service** (`systemctl status llama-turboq`): a custom-built llama.cpp
with a fixed model and launch config (`/etc/systemd/system/llama-turboq.service`,
via `start-qwopus-turboq.sh`). Hard rules, no exceptions:

- **NEVER stop, restart, edit, or reconfigure the `llama-turboq` service.**
- **NEVER spawn a parallel/second inference server** (e.g. a manual
  `llama-server` instance on :8081, another port, or another GPU process) to
  "try" another model or configuration. There is one inference service; it is
  custom-built and fixed. It is not to be changed to run different models.
- If a task needs a different model, server flag, or endpoint behavior, that is
  a **change to the service** and is only possible after **explicit human
  approval** describing exactly what changes and why. Do not act on a pre-emptive
  assumption of approval.
- If you ever find an unexpected process serving :8081 (manual `llama-server`,
  etc.), treat it as a fault you must NOT have caused; do not "fix" or swap the
  service yourself, stop what you're doing and report it.
- Benchmark/model evaluation uses the service as-is. If the fixed model cannot
  represent the population under test, that is a finding to surface, not a
  reason to reconfigure inference.

## Engineering principles (mandatory)

These are hard requirements for every change. The bar is **zero technical debt**:
leave code in better shape than you found it (Boy Scout rule), never commit a
known mess, even in adjacent code.

- **SOLID** must hold:
  - *SRP*, a class has one reason to change.
  - *OCP*, open for extension, closed for modification (add tools/backends via
    new types, not edits to the loop).
  - *LSP*, subtypes (every `Tool`/`SearchBackend`) must be substitutable.
  - *ISP*, narrow interfaces (`Tool`, `SearchBackend`, `AgentHooks`) only.
  - *DIP*, depend on abstractions (`Tool`, `SearchBackend`, `LLMClient`), not
    concretions; wiring happens at the boundary (CLI/TUI).
- **KISS / DRY / YAGNI**: no speculative generality, no duplicated logic. If you
  copy a block, extract it. If a feature isn't required now, don't add it.
- **Size limits**:
  - A class/struct definition should stay **under 200 lines of code** —
    comments and blank lines do not count, because the cap is about how much a
    type declares and deleting the comments explaining a public interface must
    never be the cheapest way to pass. Split larger types (see Audit below).
    **Gated**: `make class-size` reports the offenders and `make check` fails
    when a type grows or a new one crosses the cap
    (`tools/class_size_gate.py` + `tests/class_size_baseline.json`, currently an
    empty baseline, so it is a cliff).
  - A method/function should stay **under 10 lines** with **minimal branching**.
    Extract loops, parsing, and branching into named helpers. The **enforced**
    cap is CCN 14 / 40 lines (`make complexity`); 10 lines is the aspiration,
    and `docs/complexity-burndown.md` records the gap.
- **Layering / isolation**: this repo uses a **hexagonal (ports & adapters)**
  style, not strict N-layer:
  - *Domain core* (`lib/` + `include/agent/`) defines the ports (`Tool`,
    `SearchBackend`, `LLMClient`, `AgentHooks`) and the agent use-case. No UI,
    no `main`, no linker dependency on `tui/` or `src/`.
  - *Adapters* live in `tools/` (tool adapters), `tools/search/` (search
    backends), and the clients `src/amber` (`main.cpp`) and `tui/` (ncurses).
    Adapters depend inward on the core; the core never depends outward.
  - Keep the dependency arrows pointing at the core. If `lib/` `#include`s
    anything from `tui/`, `src/`, or `tools/` (except the tool interface
    headers), that is an isolation violation.

## Development workflow

**A feature is the unit of delivery; a commit is the unit of progress.** Work is
reviewed, gated and merged per feature. Tasks, red tests and refactors are
commits inside that feature's pull request — never their own PRs.

A commit is still a snapshot of one completed step, and its subject is the index
you scan later: `<area>: <imperative summary>`, one area per commit. The
`git-timeline` skill carries the depth; the rule belongs in the history.

### Feature, task, commit, PR

| Unit | What it is | Where it lives |
|---|---|---|
| **Milestone** | A coherent batch of features shipped together ("context shaping", "observability") | one branch, **one PR** |
| **Feature** | One user-visible change inside the milestone ("agent-proposed trims") | a commit group on that branch |
| **Task** | An issue the milestone closes | one or more commits |
| **Step** | A red test, a green fix, a refactor, a doc correction | **one commit** |
| **PR** | The milestone's review and gate unit | opened once (draft from the start, or when the first feature is reviewable) |

- **One PR per milestone.** The body lists the issues it closes (`Closes #…` once
  per task). The milestone/tracker carries task status; the PR carries the diff.
  A plan of milestones lives in `docs/plan/` (for example
  `context-shaping-milestones.md`).
- **Commit freely.** Push every completed step. A red commit followed by its
  green fix is the expected shape and survives in the PR's commit list.
- **CI is a merge gate, not a checkpoint.** A push starts the workflow; keep
  working while it runs. A red run mid-milestone is normal — the run that matters
  is the one on the completed milestone. Use `make test` / `make lint` for the
  fast local loop; do not wait three hours to learn whether one commit is fine.
- **One proposal, one sign-off, one review** for the milestone. The proposal is
  the `docs/plan/` entry or the PR description; do not re-propose per feature.
- **Squash-merge once**, when every issue the milestone closes is green and the
  PR's gates pass. `main` stays linear; one revert undoes one milestone.
- **Exceptions are about landing risk, not task size.** A security fix, a hotfix
  that must ship alone, or a change a reviewer cannot assess inside the larger
  milestone gets its own PR. "It is a small task" is not an exception — it is a
  commit, and "it is one feature" is not one either.

### Anti-pattern: one PR per task, or per feature

A PR per issue (one for the tests, one for the docs, one per file) is the
slowest way to work, and it happened here: on the compaction remediation each
task PR spent ~3 minutes of work behind ~3 hours of queueing, the feature was
never reviewable as a whole, and CI cost multiplied by the task count while
`main` gained a history of fragments. The correction — one PR per feature — still
split a single coherent batch into three PRs and three CI queues.

Detection: if the next commit is "another issue done", or "the next feature
started", it belongs on the branch you already have. If a second PR exists for
the same milestone, close it, cherry-pick its commits onto the milestone branch,
and continue there.

### Branching strategy

- **`main`** is the stable, release-ready branch. Always green. No direct pushes.
- **One branch per milestone**, named `<type>/<milestone>` (e.g.
  `fix/context-compaction`, `feat/context-shaping`), not `<type>/<micro-task>`
  and not one per feature. It lives for the milestone — days, not weeks.
- Push to the feature branch as often as work completes; open or update its PR
  and mark it draft while the feature is incomplete.
- Parallel sessions on the same feature push to the same branch and coordinate
  by file ownership (announced in the epic). They do not open sibling PRs; a
  second branch for the same feature is the anti-pattern above.

### `main` is never pushed to, force-pushed, or merged red

Four rules. The first three are enforced by branch protection
(`enforce_admins: true`, `strict` `ci-gate`, no force-push, no deletions); the fourth
is a judgement the tooling cannot make, which is why it is written down.

1. **No direct push to `main`. By anyone.** Not an admin, not a maintainer, not an
   agent working in this repository. Every change lands through a pull request.
2. **No force-push to `main`, and no history rewrite.** Not to tidy a commit, not to
   unblock a gate, not to drop a file. The escape hatch for a genuine emergency is
   `gh api -X DELETE repos/jtrefon/amber/branches/main/protection`, and **re-applying
   that protection is part of the emergency, not a follow-up** — a repository with
   protection removed will not notice that it is unprotected.
3. **Squash-merge only**, so `main` stays linear and one revert undoes one change.
4. **Never merge a pull request whose `ci-gate` is red — or whose gating jobs were
   *skipped* in a way that leaves the change unmeasured.**

   Rule 4 is the one that gets violated by accident, because every component of it
   reports success. A docs-only diff correctly skips the C++ and website jobs; `ci-gate`
   then reports SUCCESS, and a summary that reads `7 pass, 18 skipping` looks like
   everything that mattered ran. **A skip is not a pass.** Read which jobs were skipped
   and confirm each one cannot be affected by the diff, rather than counting passes.

   This is not hypothetical: on 2026-10-06 PR #222 was merged with `website-build`
   skipped, and a newly published advisory (`GHSA-68fv-2mgg-jv7q`, `source-map-js`)
   failed the next push to `main`. The gate was right and the reading of it was wrong.
   The specific failure — a gate that only runs on paths it recognises cannot protect
   the mainline from an advisory published between runs — is why there is also a
   scheduled audit job in the `always` dimension.

   **Before merging, check the tip of `main`, not just the PR.** A green PR does not
   mean `main` is green, and "all my local gates pass" is not evidence about `main`.
- Every fix or feature lives on a **feature branch** named `<type>/<short-description>`:
  - `fix/detached-thread-use-after-free`
  - `refactor/cancel-token-to-core`
  - `docs/add-tdd-policy`
- Branches are short-lived (days, not weeks). Open a **draft PR** early for
  visibility, mark it ready for review when all checks pass.
- Merge via **squash-merge** to keep `main` history clean. The squashed commit
  message must follow the imperative, scoped convention
  (e.g. `fix: cancel token now lives in core, not bash_tool globals`).

**Archived branches are prefixed `archive/`, never deleted.** 43 branches that
predate the squash-merge rule were renamed in place on 2026-10-05 rather than
deleted, and exported to
`~/Projects/amber-archive/branches-2026-10-05.bundle` (kept outside the repo so
7 MB of binary never enters git history; restore with
`git clone <bundle> amber-archive`).

The reason to keep rather than delete is a measurement, not sentiment:
`git cherry` reports a branch's commits as unique whenever a **squash**-merge
folded them into `main`, so "unique commit" does not mean "unmerged work".
`tui/god-code-decomposition` reports 31 unique commits and its artefacts are
all in `main`, because the work landed as #153. Measured across all 43, **72%
of their substantive added lines are already in `main`** (top branches 95-100%),
and the residual is doc text later rewritten — not code that never landed. 72%
is not 100%, so the branches are history, not pending work, and deleting them
on that measurement is the same mistake class the gate section above exists to
catch.

**A `git bundle` cannot capture uncommitted work.** Bundles record commits, so
anything staged or dirty in a linked worktree is invisible to one. The
`.commandcode/` files staged in `copilot-worktrees/amber/jtrefon-special-barnacle`
were therefore *not* covered by the bundle; they survived because the same
content already exists in the main worktree and `.commandcode/` is gitignored.
Capture uncommitted work as a patch or a commit, not as a branch ref.

### Feature workflow, Red → Proposal → Sign-off → Green → merge

Every bug fix and every feature follows this sequence. Steps 1 and 4 repeat per
task; steps 2, 3 and 5 happen once per feature.

```
┌──────────────────────────────────────────────────────────────┐
│  1. RED, per task. Write a failing test that reproduces the   │
│     bug or specifies the behaviour, and commit it on the      │
│     feature branch. The commit list is the proof; CI will     │
│     show the red run, but do not wait for it.                 │
│                                                              │
│  2. PROPOSAL, per feature. Draft the architecture in the      │
│     epic or PR description (see docs/fix-tracker.md),         │
│     describing the target state, not the diff.                │
│                                                              │
│  3. SIGN-OFF, per feature. The reviewer approves the          │
│     architecture before production code is written.           │
│                                                              │
│  4. GREEN, per task. Implement the fix, make the test pass,   │
│     refactor to meet the Engineering Principles, and commit.  │
│     Run local linting and static analysis every few edits     │
│     (do not batch all issues to the end). Address every       │
│     clang-tidy and cppcheck finding; zero warnings is the     │
│     threshold. Keep clangd/LSP diagnostics clean as you type. │
│                                                              │
│  5. MERGE, per feature. When every task is green, run         │
│     make clean && make && make test && make lint &&           │
│     make analyze once, let the PR's gates go green, and        │
│     squash-merge. The reviewer checks the commits against     │
│     the proposal and the whole diff as one change.            │
└──────────────────────────────────────────────────────────────┘
```

- Do NOT write production code before the failing test (step 1) exists.
- Do NOT implement without an approved proposal (step 3).
- A fix that "can't be tested" is a sign the architecture needs refactoring,
  not an excuse to skip the test.
- Lint and analysis findings are **blockers**, not suggestions. A feature PR with
  any new clang-tidy or cppcheck warning is rejected regardless of correctness.
  See the `make lint` / `make analyze` targets in the Build & verify section.
- Do not open a second PR for the next task. Add the commit.

### Code review checklist

Every PR reviewer MUST verify:

- [ ] **The PR is one feature, not one task.** The body lists every issue it
      closes; a second PR for the same epic is an anti-pattern (see above).
- [ ] **The commits tell the story.** Each task has its red test before the
      green fix; steps are separate commits, not one squash of unrelated work.
- [ ] SOLID conformance: no new SRP violations, dependency direction is correct.
- [ ] Hexagonal boundaries intact: `lib/` never `#include`s from `tui/`, `src/`,
      or `tools/` (except tool interface headers).
- [ ] Size limits: classes ≤200 lines, methods ≤10 lines with minimal branching.
- [ ] Test sequence: the PR includes a red (failing) commit followed by a green
      fix commit for each task (or a clear explanation if not possible).
- [ ] All CI checks pass: `make`, `make test`, `make lint`, `make analyze`.
- [ ] Zero dead code: no commented-out code, no stubs, no speculative branches.
- [ ] No SPDX/copyright boilerplate, first line is functional (`#include`, `#ifndef`, etc.).
- [ ] No new clang-tidy or cppcheck warnings.
- [ ] **Context is a pure stack**: only `push()`, `pop()` (LIFO), `clear()`, `get_all()`. No mutation of sealed messages. No `replace()` or similar. The FNV-1a hash chain in `get_all()` asserts integrity, any bypass crashes in debug.

## Prompting philosophy (mandatory)

Prompts are **descriptive, not prohibitive**: describe the role, personality,
environment and tooling, and empower the agent to work, never force or
forbid behavior ("never", "don't", "must", "do not" are banned from
`prompts/`). Conventions (like the closing `done` marker) are described as
the natural shape of finished work, not commands. A prompt change is a
behavior change: prove it with a before/after benchmark run (see
`BENCHMARK.md` "Prompt v2" section for the template).

## Coding standards

- **RAII**: ownership follows resource acquisition. Use `unique_ptr` for
  exclusive ownership, scoped objects on the stack, and `shared_ptr` only when
  ownership is genuinely shared. Never use raw `new`/`delete`.
- **Rule of Five / Zero**: prefer Rule of Zero (implicit special members are
  correct). When a destructor, copy constructor, copy assignment, move constructor,
  or move assignment is user-defined, explicitly declare all five or `= delete`.
- **`noexcept`**: mark pure accessors, trivial getters, and functions that
  never throw as `noexcept`. Only omit `noexcept` when the function legitimately
  throws. Every `Tool::name()`, `is_read_only()`, `requires_approval()`,
  `SearchBackend::name()`, `StreamDecoder::prompt_tokens()` should be `noexcept`.
  The accessors returning `std::string` are `noexcept` **by convention** even
  though allocating the result can throw `bad_alloc`: the convention is
  deliberate, so a mutating or allocating function that is *not* an accessor
  (`Context::push`, `parse_status`, `current_platform`, a `main()`, a
  destructor) must not claim `noexcept` — that is the case the rule forbids.
  `bugprone-exception-escape` cannot tell the two apart and is disabled for it
  (`.clang-tidy`), see `docs/issues.md` CX9.
- **Const-correctness**: mark member functions and parameters `const` wherever
  possible. Use `const&` for read-only parameters of non-trivial types.

## Error handling conventions

- **Tools**: always return errors via `ToolResult{false, "", error_msg}`.
  Never throw from `Tool::execute()`. Catch unexpected exceptions and convert
  to `ToolResult`.
- **Library functions**: may throw `std::runtime_error` for truly exceptional
  conditions (transport failure, corrupt config). Do not throw for expected
  states (empty results, missing files), return an error code, empty optional,
  or `ToolResult`.
- **Recoverable errors**: model errors (malformed JSON, HTTP 4xx/5xx) should
  be returned as assistant messages or error-flagged `ToolResult` so the LLM
  can self-recover.
- **Unrecoverable errors**: configuration corruption, libcurl init failure.
  Throw at construction; the host (CLI/TUI) catches and reports.
- **Assertions**: use `assert()` only for invariants that should never fire
  in a correct program. Never use asserts for input validation.

## TDD / Red-Green-Refactor (mandatory)

- **Bug fixes** must start with a failing test that reproduces the bug. Only
  then is the production code changed (Red → Green). After the fix passes,
  the test is committed alongside the fix.
- **New features** must follow the same cycle: write a failing test that
  specifies the desired behaviour, implement until green, then refactor.
- **Coverage threshold**: new code paths must have ≥80% line coverage. The CI
  gate (`make test`) must pass before merge.
- **Hermetic tests**: mock the LLM by testing the pure seams
  (`Dialect::parse_models_response` / `parse_completion` / `context_overflow_hint`,
  `merge_server_info`) directly; do not hit a live server in the unit suite.
- **Test granularity**: prefer many small `TEST(name)` blocks over a single
  large test function. Each test exercises one behaviour.
- **Test location**: behaviour changes go in `tests/run_tests.cpp`. New test
  files may be added for major modules (`tests/compressor_test.cpp`,
  `tests/agent_test.cpp`), add them to `UNITTEST_OBJ` in `Makefile`.

## Design patterns in use

- **Strategy**: `SearchBackend` (`grep` vs `semantic`), selected at runtime by
  the `search` tool's `mode` arg without changing the schema.
- **Strategy + Registry (provider wire protocols)**: `Dialect` implementations
  (`openai`, `anthropic`, …) are selected once per client from `Config::flavor`
  through the `make_dialect` registry; the transport, agent loop, and UIs never
  branch on the protocol. Adding a provider protocol is one dialect file + one
  registry row (`docs/spec/llm-client/dialect.md`).
- **Factory**: `make_*_tool()` / `make_*_backend()` free functions return
  `unique_ptr<>` so the registry owns distinct instances; `register_default_tools`
  wires the standard set for every host.
- **Registry / Service Locator**: `ToolRegistry` owns and looks up tools by
  name for the agent loop and the LLM `tools[]` schema.
- **Observer**: `AgentHooks` (via `std::function` callbacks) lets UIs observe
  the agent loop without the core knowing about them. More precise than "Template
  Method" since the hooks are set, not subclassed.
- **Command**: `ProcessStartTool` / `ProcessReadTool` / `ProcessStopTool` each
  encapsulate a background-process request as an object with a uniform `execute()`.
- **Protection Proxy**: `Workspace::confine()` guards filesystem access behind
  path-confinement checks, proxying the real filesystem.
- **Marker/Tag**: `Message::harness` tags the notices the harness itself authors
  (`[harness]` retry summaries, repair reports, budget and stop reasons) so the
  loop never mistakes one for the model's own reply and a UI can render it as
  the environment speaking.
- **Memento**: `run_compression()` (gate path) and `compress_now()` leave the
  live `context_` untouched when the pipeline fails (spec invariant 7); the
  rebuild via `clear()` + `push()` only happens on success, capturing and
  rolling back state atomically.
- **Adapter**: `LLMClient` adapts libcurl + the configured `Dialect` behind a
  small C++ interface; each `Dialect` adapts one provider wire protocol;
  `Workspace` adapts filesystem confinement behind a simple `confine()` port.
- **Facade**: `Agent` orchestrates client + registry + hooks + log into one
  `run()` use-case.
- **Pub/Sub**: `EventBus` (plugin framework) provides typed event subscription with
  intercept (modify/cancel) and observe (read-only) semantics. Plugins subscribe
  to agent lifecycle events without coupling to the agent loop.
- **Capability**: `IPlugin` declares `Capability` objects (tool, provider, hook,
  theme, completion, memory, search) that the `PluginRegistry` registers into
  the appropriate subsystem. New capability types extend without modifying the
  registry (OCP).

## Architecture audit (status: NON-CONFORMING on size limits)

Last reviewed against the limits above. The architecture and SOLID posture are
**sound** (clean hexagonal boundaries, correct abstraction, no core↔UI coupling),
but several files exceed the hard size limits and must be split before we can
claim 0-debt conformance. Line counts below are enforced by
`tests/build_hygiene.sh` (`make check`), if they drift, refresh the table:

| File | Lines | Issue |
|------|------:|-------|
| `tests/run_tests.cpp` | - | Test file; exempt from class-size rule but a candidate for per-area headers. |
| `lib/session.cpp` | 321 | Resolved, `list()` now uses `std::filesystem::directory_iterator`. |
| `tui/tui_render.cpp` | 125 | Method implementations (not a class); exempt from class-size rule; real rendering now in `render_engine.cpp` (FIX-026). |
| `tui/tui_input.cpp` | 4497 | Method implementations (not a class); exempt from class-size rule. |

### Resolved
- `lib/llm.cpp` (511 → 84): split into `stream_decoder` (formerly `sse_parser`),
  `dialect_openai` (the OpenAI wire format), `http_transport`, `model_probe`,
  `debug_log` (+ `llm.cpp` keeps the class). Provider wire protocols now live in
  `lib/dialect_*.cpp` behind the `Dialect` port, see
  `docs/spec/llm-client/dialect.md`.
- `lib/agent.cpp` (473 → 200, now ~1000): `run` decomposed into `advance_turn`,
  `fold_reply`, `recover_internal_failure`, `dispatch_tool_calls`,
  `agent_helpers`, `tool_recovery`; `compress_now` delegates to
  `CompressionPipeline::compress()` via `compression_->compress()`. The old
  `confirm_turn` probe was retired on 2026-10-18 (the turn contract: a reply
  with no tool calls ends the turn).
- `tui/tui.cpp` (1245 → 171, now 1044): god-class `Tui` split into `tui.h` (declaration,
  +420 lines) + `tui_render.cpp`, `tui_input.cpp`, `tui_session.cpp`,
  `tui_main.cpp`. No file defines a class >200 lines.
- `tui/widgets.cpp` (333): split into `dialog.cpp`, `form_edit.cpp`,
  `info_dialog.cpp`, `menu_select.cpp`.
- `tools/search/semantic_backend.cpp` (227 → 126): free helpers extracted to
  `semantic_index.cpp` (107 lines) + `semantic_helpers.h`.
- `tools/bash_tool.cpp` (191 → 196): `execute()` decomposed into free helpers
  `run_with_timeout` + `drain_output` in the anonymous namespace.
- **Detached thread in `chat_once`** (Critical): replaced with synchronous
  extraction, `chat_once` no longer spawns a thread.
- **HTTP transport + tool-cancel globals** (Critical): `CancellationToken` in
  `include/agent/process.h`, used by `http_transport`; no module-level globals.
- **`Agent::run()` SRP** (High): decomposed into 4 named methods.
- **`Agent::compress_now()` SRP** (High): reuses `CompressionPipeline::compress()`
  via `compression_->compress()` with `CompressionObserver`.
- **Tool cancel globals** (High): instance-scoped `CancellationToken`.
- **Tools in `libagent.a`** (High): split into `libagent_core.a` +
  `libagent_tools.a` in `Makefile`.
- **Tests include TUI headers** (Medium): TUI tests moved to
  `tests/tui_tests.cpp`; `tests/run_tests.cpp` is TUI-header-free.

All items previously listed in "Current outstanding issues" have been resolved.
See `docs/issues.md` for the historical register and `docs/fix-tracker.md` for
fix details.

When refactoring to fix these, preserve behavior and keep `make test` green. Run
`make clean && make` after touching headers (see Compilation gotchas).
