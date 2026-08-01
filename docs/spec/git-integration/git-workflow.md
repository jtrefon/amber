## Spec: Git Workflow Integration

### Purpose

Give the agent structured awareness of git so it can snapshot work incrementally,
review history, and revert changes safely — all through the existing bash tool.
No new tool or agent-loop code is required. The integration is purely
prompt-driven and front-end configured.

### Ownership

- **Workflow prompt**: `prompts/git.md` — loaded as optional third prompt
  alongside `system.md` and `tools.md`
- **Config key**: `git_prompt` in `amber.conf`, overridable via `AMBER_GIT_PROMPT`
  env var
- **Shell prompt**: `scripts/amber-prompt.zsh` — standalone zsh theme, not part
  of the agent
- **Source files**: `lib/agent.cpp` (prompt loading in `ensure_system_prompt()`),
  `lib/config.cpp` (config key parsing), `include/agent/config.h` (field)
- **Test files**: none — tested implicitly by the compression integration test
  (verifies prompt loading doesn't crash)

---

### Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Three-layer design                        │
├──────────────┬──────────────────────┬────────────────────────┤
│  Layer       │  What                │  How                   │
├──────────────┼──────────────────────┼────────────────────────┤
│  Shell       │  amber-prompt.zsh    │  precmd hook in zsh    │
│  (user)      │  project/branch/diff │  sourced from .zshrc   │
├──────────────┼──────────────────────┼────────────────────────┤
│  Agent       │  prompts/git.md      │  Appended to system    │
│  (model)     │  git workflow guide  │  prompt at startup     │
├──────────────┼──────────────────────┼────────────────────────┤
│  Execution   │  bash tool           │  All git commands run  │
│  (runtime)   │  git status/diff/    │  through existing bash │
│              │  add/commit/revert   │  tool with timeouts,   │
│              │                      │  output caps, approval │
└──────────────┴──────────────────────┴────────────────────────┘
```

**No dedicated git tool.** Git operations run through the existing `bash_tool`
which already provides idle timeout, 64 KiB output cap, cancellation, and
approval gating. Adding a wrapper around `git` would duplicate infrastructure
with zero benefit.

**No auto-commit hook in code.** The agent decides when to commit, guided by
the git prompt. This keeps the architecture simple and does not constrain
the model's judgment about what constitutes a "logical unit of work."

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Git commands (`git status`, `git diff`, `git log`, `git add`, `git commit`, `git revert`) via bash tool |
| **Output** | Standard git CLI output captured in `ToolResult.output` |
| **Error states** | Non-git directory, merge conflicts, dirty index, permission denied — all surfaced by git CLI exit codes |
| **Invariants** | See below |
| **Thread safety** | Git operations block the calling thread (same as bash tool). No concurrent git operations |

### Invariants

1. **No dedicated git tool exists.** All git operations use the bash tool.
   Adding a `git_tool` is a deliberate future decision, not an accident of
   extraction.
2. **Auto-commit is prompt-driven, not code-enforced.** There is no
   `Agent::auto_commit()` or commit hook in the agent loop. The model chooses
   when to commit based on prompt guidance.
3. **Empty commits are never created.** The git prompt explicitly warns against
   committing when `git diff --stat` shows zero changes.
4. **Commit messages are descriptive.** The prompt requires both *what* changed
   and *why*, using scoped subjects (`area: description`).
5. **Rollback uses `git revert`, not `git reset`.** Reset rewrites history and
   is unsafe for collaborative workflows. Revert creates a new commit that
   undoes the target, preserving history.
6. **The shell prompt (`amber-prompt.zsh`) is independent of the agent.**
   It is sourced in `.zshrc`, reads git state from the filesystem, and has
   no communication with the amber process.

---

### Components

#### A. Workflow prompt (`prompts/git.md`)

Loaded as part of the system prompt on agent startup. Appended after the mode
section in `ensure_system_prompt()`:

```
ensure_system_prompt():
  system  = load("system.md")
  system += load("tools.md")  (or rendered tool registry)
  system += mode_footer()     ("WRITE mode" / "READ mode" / "YOLO mode")
  system += load("git.md")    ← optional, no crash if missing
  push(system)
```

The git prompt is optional — if the file is missing or empty, it is silently
skipped. No agent behaviour changes without it.

**Content design** (see `prompts/git.md` for the current text):

- **Review before changes**: `git status`, `git diff --stat`, `git log --oneline`
- **Commit workflow**: `git add -A` + `git commit -m "scope: message"`
- **Commit quality**: only when changes exist, descriptive what+why, scoped
  subjects, granular enough for independent revert
- **Rollback**: `git log` to find the target, `git revert <sha>` to undo,
  `git diff <sha>~1 <sha>` to inspect before reverting

#### B. Shell prompt (`scripts/amber-prompt.zsh`)

Standalone zsh `precmd` hook. Sourced from `.zshrc`, not launched by amber.
Has zero interaction with the agent process.

**Format** (three styles, selected by `AMBER_PROMPT` env var):

| Style | Example |
|-------|---------|
| `deluxe` (default) | `amber fix/tui-ux-overhaul +3/-1 ❯` |
| `minimal` | `fix/tui-ux-overhaul +3/-1 %` |
| `full` | Two-line frame with ┌─╰ characters |

**Components displayed:**

| Component | Color | Source |
|-----------|-------|--------|
| Project name | Yellow | Hardcoded `amber` |
| Git branch | Cyan | `git symbolic-ref HEAD` |
| Lines added | Green | `git diff --shortstat` parsed |
| Lines deleted | Red | `git diff --shortstat` parsed |

The prompt only renders when `git rev-parse --show-toplevel` succeeds (i.e.,
the cwd is inside a git repo). Outside git repos, it falls back to
`amber % ` with no branch/diff info.

#### C. Git tool consideration (deferred)

A dedicated `git_tool` could be added in the future if:

1. The bash tool's 64 KiB output cap truncates large `git log` or `git diff`
   listings that the agent needs to read in full.
2. Structured output (JSON machine-parseable logs) is needed for agent
   decision-making.
3. The approval gate needs to differentiate git operations from arbitrary
   shell commands (e.g., approve commits but deny arbitrary bash).

None of these conditions are met today, so no git tool exists.

---

### Scenarios

#### [GW-01] Agent reviews state before making changes

- **Given**: An active git repository, no unstaged changes
- **Input**: git status via bash tool
- **Expected**: `git status` output shows clean working tree
- **Rationale**: The git prompt recommends reviewing state before any edit

#### [GW-02] Agent commits after completing a logical change

- **Given**: One or more files were modified by the write/edit tool
- **Action**: `git add -A` + `git commit -m "scope: description"`
- **Expected**: Commit created with descriptive message. `git diff --stat`
  confirms changes were committed. Exit code 0.
- **Guard**: If `git diff --stat` is empty, the agent must NOT commit.

#### [GW-03] Agent reverts a bad commit

- **Given**: A previous commit introduced a problem
- **Action**: `git log --oneline -10` to find the target, then
  `git revert <sha>`
- **Expected**: A new commit is created that undoes the target. History is
  preserved. No force-push needed.
- **On failure**: If revert has conflicts, the agent must not proceed blindly —
  it should report the conflict and ask the user.

#### [GW-04] Agent runs in a non-git directory

- **Given**: cwd is not a git repository
- **Action**: `git status` returns exit 128 and `"fatal: not a git repository"`
- **Expected**: Agent recognizes the error and does not attempt further git
  operations. No crash or infinite retry.
- **Recovery**: `git init` if the project is new, or report the situation.

---

### Cross-references

- **Depends on**: `docs/spec/tools/bash-tool.md` (git commands execute via bash),
  `docs/spec/config/file-config.md` (`git_prompt` key)
- **Depended on by**: none (independent layer)
- **Active files**: `prompts/git.md`, `scripts/amber-prompt.zsh`,
  `include/agent/config.h`, `lib/config.cpp`, `lib/agent.cpp`

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-07-30 | Initial spec — prompt-driven git integration, no dedicated tool |
