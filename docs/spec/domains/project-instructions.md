## Spec: Project Instructions

### Purpose

Carry the project's own standing rules (`AGENTS.md`) in the system prompt, and
name the nested instruction files so an agent working inside a package reads the
nearest one. The project's declared rules outrank the harness prose; a direct
instruction from the user outranks both.

### Ownership

- **Source files**: `plugins/project_instructions/project_instructions_plugin.cpp`
  (the plugin, the cap, the nested scan), `lib/worktree.cpp`
  (`git_files_matching`).
- **Test files**: `tests/plugin_runtime_test.cpp` (block, cap, scan),
  `tests/worktree_test.cpp` (git enumeration).

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | The workspace root; `AGENTS.md` at its top; a git checkout or a plain tree |
| **Output** | One System prompt block: the root file's body, truncated at 8 KB with a marker and a table of contents of the omitted top-level sections, plus the nested files' repo-relative paths |
| **Error states** | No root file → no block. Unreadable file → no block. Non-repository → filesystem walk |
| **Invariants** | See below |
| **Thread safety** | The block is built once (`load_once`) on the agent thread; a compression completion refreshes it |

### Invariants

1. **The injected body is capped at 8 KB**, cut at the last line boundary inside
   the cap, followed by `[truncated at 8 KB — read AGENTS.md for the rest]` and
   a digest of every top-level `##` section whose heading is past the cut:
   `- <title> (lines <start>-<end>)`, the range ending at the next heading with
   trailing blank lines trimmed. A section whose heading is inside the body is
   not repeated. Measured on this repo's AGENTS.md: the cut lands around line
   116, the digest names 18 sections in 936 characters (~0.9 KB), so the block
   is the 8 KiB body plus ~1 KB — the reason the cap was not raised. The body
   size is a per-turn cost, so it does not grow silently.
2. **Nested files are named, never inlined.** The nearest file wins; the agent
   reads the one that applies to the tree it works in.
3. **Nested discovery honours the project's ignore rules.** In a checkout the
   scan is `git ls-files --cached --others --exclude-standard -- ':(glob)**/AGENTS.md'`:
   tracked files plus untracked files git does not ignore.
4. **A nested repository or linked worktree is never entered.** Git does not
   descend into one; `under_nested_repository()` additionally checks for a
   `.git` entry in the path's ancestry, which covers a worktree whose metadata
   points elsewhere and git implementations that do list it.
5. **The scan is bounded.** At most `kMaxNested` (12) names are rendered; the
   fallback walk also skips hidden directories, `node_modules`, `build`,
   `target`, `dist`, and stops below depth 4.
6. **Without git the walk keeps the same boundaries**: hidden/build directories
   and any directory carrying `.git`.

---

### Scenarios

#### [PI-01] The root file reaches the prompt

- **Given**: `AGENTS.md` at the workspace root
- **Expected**: its body is in a System block, with the precedence statement
  ("outrank this system prompt").

#### [PI-02] A nested file is named, not inlined

- **Given**: `apps/web/AGENTS.md`
- **Expected**: `apps/web/AGENTS.md` appears under "Nested instruction files";
  its body does not.

#### [PI-03] A gitignored tree is not named

- **Given**: a repository whose `.gitignore` excludes `ignored/`, and
  `ignored/AGENTS.md` exists
- **Expected**: the ignored file is absent from the block.

#### [PI-04] A linked worktree is not named

- **Given**: a checkout containing a linked worktree (`sibling/` with a `.git`
  file) and `sibling/AGENTS.md`
- **Expected**: the sibling file is absent, whether git skipped it or the
  ancestry check did.

#### [PI-05] A non-git tree still skips nested repositories

- **Given**: a plain directory with `pkg/AGENTS.md` and `sibling/.git` +
  `sibling/AGENTS.md`
- **Expected**: `pkg/AGENTS.md` is named; `sibling/AGENTS.md` is not.

#### [PI-06] The omitted sections are named

- **Given**: an `AGENTS.md` larger than 8 KB whose top-level `##` sections
  continue past the cut
- **Expected**: the marker is followed by `Omitted top-level sections (line
  ranges):` and one `- <title> (lines a-b)` per section whose heading is past
  the cut. A heading inside the body is not listed again; each range ends at
  the next heading with trailing blank lines trimmed.

#### [PI-07] No instructions, no block

- **Given**: no root `AGENTS.md`
- **Expected**: the plugin contributes no block (and disabling it removes the
  block it did contribute).

---

### Cross-references

- **Depends on**: `workspace/path-confinement.md` (the workspace root),
  `agent-loop/prompt-assembly.md` (System placement).
- **Depended on by**: `domains/project-index.md` (the index is derived material;
  the project's declared file outranks it).

### Revision history

| Date | Reason |
|------|--------|
| 2026-10-19 | Nested discovery is git-aware: ignore rules honoured, nested repositories and linked worktrees excluded, cap and fallback documented (#280) |
| 2026-10-20 | The 8 KB cut is followed by a digest of the omitted top-level sections (title + line range); measured cost recorded (#284) |
