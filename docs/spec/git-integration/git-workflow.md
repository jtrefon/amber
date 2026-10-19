## Spec: Git Workflow Integration

### Purpose

Give the agent a timeline it can trust: every completed step is one commit
whose subject is a compact index key, and history can be scanned, pinpointed
and rolled back with git itself. The integration stays prompt- and
skill-driven; git runs through the bash tool, which already has approval,
timeouts, cancellation and output caps.

The one thing the model cannot infer is the workspace it is in: an amber window
works in a **detached** worktree, and commits that no branch reaches are refused
at window close. The harness states that in the prompt instead of letting the
agent discover it from the refusal.

### Ownership

- **Agent layer**: `prompts/domains/coding/git.md` (resident rule, coding
  domain) and `plugins/git_timeline/` (the `git-timeline` skill: the playbook).
- **Host layer**: `tui/textutil.cpp` (`git_prompt`, `git_branch_label`) and
  `tui/render_engine.cpp` (`GitState`: project, branch/@oid, diff, dirty).
- **Workspace layer**: `lib/worktree.cpp` (`create_worktree`,
  `remove_worktree_safe`, `worktree_session_note`) and `lib/agent.cpp`
  (the note in the system prompt).
- **Data layer**: `lib/prompt.cpp`, `lib/data_path.cpp` and the install rule —
  packaged installs find the prose, the per-tool docs and the coding library.
- **Execution**: the bash tool. There is no git tool.
- **Tests**: `tests/worktree_test.cpp`, `tests/agent_loop_test.cpp`,
  `tests/plugin_runtime_test.cpp`, `tests/plugin_docs_test.cpp`,
  `tests/tui_tests.cpp`.

---

### Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Three layers                              │
├──────────────┬──────────────────────┬────────────────────────┤
│ Host         │ git prompt (input    │ project · branch/@oid  │
│ (user)       │ line) + GitState     │ · +ins/-del · dirty *  │
├──────────────┼──────────────────────┼────────────────────────┤
│ Agent        │ coding git.md        │ resident rule          │
│ (model)      │ git-timeline skill   │ the playbook on demand │
├──────────────┼──────────────────────┼────────────────────────┤
│ Execution    │ bash tool            │ every git command      │
└──────────────┴──────────────────────┴────────────────────────┘
```

**No dedicated git tool.** `git status/diff/log/add/commit/revert/…` run through
the existing bash tool, which already provides the idle timeout, the 64 KiB
output cap, cancellation and the approval gate. A wrapper would duplicate that
infrastructure and buy nothing.

**No harness-injected commit and no injected turn.** The model decides when a
step is complete and commits it with a subject it wrote. The harness observes
(branch, diff, dirty) and refuses to discard unreachable commits; it never
fabricates a continuation to ask for a snapshot.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Git commands via the bash tool; the workspace note in the system prompt; the `git-timeline` skill body on `read_skill` |
| **Output** | Commits with an indexed subject; standard git output in `ToolResult.output`; a prompt line naming a detached worktree |
| **Error states** | Non-repository, detached HEAD, dirty/ unreachable worktree at close, revert conflicts, missing data files (bootstrap), a packaged install without the library (now installed) |
| **Invariants** | See below |
| **Thread safety** | Git commands run on the tool's worker. The workspace note is computed when a window's root is set (UI thread, two short git calls) and cached; prompt assembly reads the cached string |

### Invariants

1. **No dedicated git tool.** Adding one is a deliberate future decision.
2. **Commits are model-driven.** The harness never commits and never injects a
   turn to ask for one.
3. **The subject is the index key.** `<area>: <imperative summary>`, at most 72
   characters, **one area per commit**. A change spanning two unrelated areas is
   two commits.
4. **No empty commits.** A snapshot with nothing in it means the step is not
   done.
5. **Rollback is chosen by how the history is shared.** `git revert` for
   anything pushed or shared; `git reset --soft`/`--mixed` only for a local,
   unpushed snapshot. `--hard` and rewrites of pushed history are the user's
   decision, not the agent's.
6. **A detached worktree gets a branch before its first commit.**
   `remove_worktree_safe` refuses to close a window whose HEAD's commits are
   reachable from no ref.
7. **A packaged install is a first-class layout.** The domain prose, per-tool
   docs and the coding library resolve through `data_file_candidates`
   (`<prefix>/bin` → `<prefix>/share/amber`), and `make install` ships the
   library tree.

---

### The snapshot step

The model takes one snapshot when a step is complete, inside the turn, via bash:

```
git add <named paths>
git commit -m "<area>: <what changed>"
git log --oneline -1        # the key exists
git status --short          # nothing was left behind
```

Naming paths keeps unrelated work and stray credentials out. The verification
pair is part of the step: a dirty tree at the end of a turn is work the
timeline did not capture.

### History traversal

| Question | Command |
|---|---|
| What are the recent snapshots? | `git log --oneline --no-merges -50` |
| Which ones touched this area? | `git log --oneline --grep '^tui:'` |
| When did this string appear? | `git log -S'<string>' -- <path>` |
| Which snapshot owns these lines? | `git blame -L <start>,<end> <file>` |
| What did a function's history look like? | `git log -L :<fn>:<file>` |
| Which snapshot broke it? | `git bisect start/good/bad` + `git bisect run <check>` |

The index is the entry point: the bad commit's subject names the change to read
first, and the path/pickaxe queries narrow from there.

### Rollback ladder

| State | Route | Why |
|---|---|---|
| Uncommitted edits | `git restore <paths>` (or `git stash`) | nothing to rewrite |
| Local snapshot, unpushed | `git reset --soft` / `--mixed <rev>` | keeps the work, drops the commit |
| Pushed, shared, or the project's mainline | `git revert <rev>` | an undo commit; history others built on stays intact |
| History to inspect first | `git show <sha>`, `git diff <a>..<b>` | the decision needs the delta |

`--hard` discards uncommitted work; rewriting pushed history changes what
others build on. Both are the user's call. A revert conflict is reported, not
guessed at.

---

### Scenarios

#### [GW-01] A completed step becomes one indexed snapshot

- **Given**: a worktree with edits for one area
- **Action**: `git add <paths>` + `git commit -m "area: summary"`
- **Expected**: one commit whose subject matches `<area>: <summary>`; `git log
  --oneline -1` shows it; `git status --short` is empty.

#### [GW-02] Two unrelated areas are two commits

- **Given**: edits to two unrelated areas in one turn
- **Action**: the agent stages and commits them separately
- **Expected**: two subjects, each naming one area. A single subject covering
  both is the failure.

#### [GW-03] A detached worktree is named in the prompt

- **Given**: a window workspace created detached at `<oid>`
- **Expected**: the system prompt carries the worktree note with the short oid
  and the `git switch -c` rule; the note disappears once a branch exists.

#### [GW-04] Closing a window refuses unreachable commits

- **Given**: a worktree whose HEAD commit is on no branch
- **Action**: close the window
- **Expected**: `remove_worktree_safe` refuses, and the notice says commits are
  on no branch. With a branch, removal succeeds.

#### [GW-05] A shared snapshot is reverted, not rewritten

- **Given**: a bad commit that is already pushed
- **Action**: `git revert <sha>`
- **Expected**: a new commit undoes it; history is intact.

#### [GW-06] A local snapshot is reset

- **Given**: a bad commit that exists only locally
- **Action**: `git reset --soft <sha>`
- **Expected**: the work is back in the index, the commit is gone, no other
  history is touched.

#### [GW-07] Bisect pinpoints the snapshot

- **Given**: a regression with a known-good and known-bad point
- **Action**: `git bisect run <check>`
- **Expected**: the first bad commit is reported, and its subject identifies the
  change.

#### [GW-08] Untracked-only work is visible as dirty

- **Given**: a tree whose only changes are new untracked files
- **Expected**: `git diff` shows nothing, and the prompt's `*` marker shows the
  work is not captured.

#### [GW-09] A hundred keys scan by area

- **Given**: a long history
- **Action**: `git log --oneline --grep '^tui:'`
- **Expected**: the matching snapshots, one line each; no need to read the full
  log.

#### [GW-10] A packaged install finds its data

- **Given**: `make install` into a prefix (`bin/` + `share/amber/`)
- **Expected**: system.md, the coding prose, the per-tool docs and the library
  all resolve from an empty CWD; the release smoke asserts the library tree is
  in the package.

---

### Cross-references

- **Depends on**: `docs/spec/tools/bash-tool.md` (execution),
  `docs/spec/workspace/path-confinement.md` (workspace root),
  `docs/spec/skills/agent-skills.md` (activation tier),
  `docs/spec/domains/coding-library.md` (the library the index selects from).
- **Depended on by**: `docs/spec/INDEX.md`.
- **Test coverage**: `tests/worktree_test.cpp` (note, removal guards),
  `tests/agent_loop_test.cpp` (note in the prompt),
  `tests/plugin_runtime_test.cpp` (skill install/unwind, body grammar),
  `tests/plugin_docs_test.cpp` (resident budget, one-home rule),
  `tests/tui_tests.cpp` (branch label).

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-30 | Initial spec — prompt-driven git integration, no dedicated tool |
| 2026-10-19 | Snapshots with an indexed subject; the git-timeline skill; detached-worktree rule; rollback ladder amended (reset for local unpushed, revert for shared); packaged data resolution |
