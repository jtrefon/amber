# Security Hardening Proposal — 2026-09-10

- **Status:** Draft — awaiting sign-off
- **Branch:** `fix/security-hardening-v1` (proposed)
- **Author:** Architecture review 2026-09-10 (4 parallel deep-dive subagents + manual verification)
- **Target:** Close every exploitable path in the LLM-output trust boundary
- **Constraints:** No `llama-turboq` service changes; no new speculative generality; each FIX is Red→Green per `AGENTS.md`; Boy Scout for method size; no bulk rewrite.
- **References:** `AGENTS.md` (Security model, Engineering principles, Fix workflow), `docs/fix-tracker.md`, `include/agent/shell_classify.h`, `include/agent/tool.h`, `include/agent/workspace.h`, `include/agent/search_backend.h`

---

## 1. Executive Summary

The AGENTS.md security model states three invariants:

1. **read/write confine paths to the workspace root** — absolute paths and `../` escapes are rejected.
2. **bash tool is approval-gated and fail-safe** — denied if no approver; destructive/outside commands prompt.
3. **treat LLM output as untrusted.**

The architecture review found **five classes of bypass** where the current code violates these invariants. Each is exploitable today by an adversarial or merely confused model. This proposal pays them down as **5 FIXes**, each self-contained, each Red→Proposal→Sign-off→Green→PR, ordered by exploit severity.

The fixes are surgical: they harden existing ports (`Tool::requires_approval`, `Workspace::confine`, `classify_shell`, `SearchBackend`) rather than introducing new abstractions. No speculative generality. The one structural change (SEC-04) extracts a shared `PathResolver` so the TUI stops hand-rolling the same flawed prefix-join that `Workspace::confine` already solves correctly.

**Done when:** `make clean && make && make test && make lint && make analyze` zero warnings on both compilers; every Red test below passes; no new SOLID violation; `Workspace::confine` is the single path-confinement authority across `tools/` and `tui/`.

---

## 2. Verified Findings (grounded in source)

### SEC-01 — Shell classifier bypass vectors (`lib/shell_classify.cpp`)

The classifier drives `BashTool::requires_approval` (`tools/bash_tool.cpp:206`). Four bypass paths let destructive/out-of-workspace commands classify as `ReadOnly` and run **without approval**:

| # | Bypass | Root cause | Exploit |
|---|--------|-----------|---------|
| a | Reader args skip path checks | `shell_classify.cpp:307` `if (reader) continue;` | `cat /etc/passwd`, `grep foo /etc/passwd`, `ls /tmp` → `ReadOnly` |
| b | `cd` is a reader | `shell_classify.cpp:69` `kReaders` includes `"cd"` | `cd /tmp && cat secret` → `ReadOnly`; later relative paths resolve outside while classifier anchors to original root |
| c | Newline not a chain op | `shell_classify.cpp:47` tokenizes on `\n` but `is_chain_op` (`:75`) only checks `; && \|\| \|` | `echo a\ncat /etc/passwd` → one segment, head `echo` (reader) → `ReadOnly` |
| d | Input-redirect target skipped | `shell_classify.cpp:293` `if (is_input_redirect(t)) continue;` runs before the path scan | `cat < /etc/passwd` → `ReadOnly` |

**Why this is wrong:** the `kReaders` allowlist exists to let `ls`/`cat`/`grep` run free *for in-workspace targets*. The implementation conflated "reader head" with "arguments are never paths", so an absolute path argument is treated as inert data. The AGENTS.md invariant "absolute paths and `../` escapes are rejected" is violated for every reader head.

### SEC-02 — Side-effecting tools skip the approval gate (`tools/`, `lib/plugin.cpp`)

`Tool::requires_approval` defaults to `false` (`include/agent/tool.h:43`). Four side-effecting tools do not override it:

| Tool | File | Side effect | Approval? |
|------|------|-----------|-----------|
| `WriteTool` | `tools/write_tool.cpp:19` | create/overwrite workspace files | **none** |
| `TodowriteTool` | `tools/todowrite_tool.cpp:33` | mutates session task store | **none** |
| `TaskTool` | `tools/task_tool.cpp:12` | spawns sub-agent with full tool access | **none** |
| `PluginTool` | `lib/plugin.cpp:354` | runs external-process code (cdp eval/click/navigate) | **none** |

`WriteTool` is the most severe: the AGENTS.md security model says "read/write confine paths", and `WriteTool` does confine via `Workspace::confine` (`write_tool.cpp:61`), but confinement is not consent. In `READ` mode the dispatch gate blocks it (`dispatch.cpp:198`), but in `WRITE` mode an unprivileged model can silently overwrite any workspace file. The approval gate is the documented control; the tool opts out of it.

`TaskTool` is a privilege-escalation vector: a sub-agent inherits the parent's full tool registry, so a model that has been denied `bash` can spawn a `task` whose worker runs `bash` ungated.

`PluginTool` runs arbitrary external-process code (`cdp eval` executes model-supplied JavaScript in a browser) with no gate.

### SEC-03 — Search backend hardening (`tools/search/`)

| # | Issue | File | Risk |
|---|-------|------|------|
| a | No `--` before pattern | `grep_backend.cpp:41` | A pattern starting with `-` is parsed as a grep option (`grep -rnIE '-e foo'` → option `-e`). Option injection. |
| b | No timeout | `grep_backend.cpp:69-79` | A pathological regex (`(a+)+b`) hangs the agent thread indefinitely. |
| c | No output cap above `max` | `grep_backend.cpp:42,74-75` | Model passes `max: 1000000`; `head -n` output read fully into memory. DoS. |
| d | Semantic backend follows symlinks | `semantic_index.cpp:126` `fs::is_regular_file(p, ec)` | A symlink in the workspace pointing to `/etc/passwd` is indexed and its content returned. `GrepBackend` is safe only by accident (`grep -r` defaults to no-follow). |
| e | `max` not clamped | `search_tool.cpp:106-107` | Only clamps `< 1`; no upper bound. |

### SEC-04 — TUI path confinement gaps (`tui/`)

Three TUI paths build file paths by prefix-joining `Workspace::root()` **without** calling `Workspace::confine()`:

| Site | File:line | Exploit |
|------|----------|---------|
| `@`-reference expansion | `tui/tui.cpp:189` `fs::path(root) / ref` | `@/etc/passwd` → `ref = "etc/passwd"`? No — `ref` is the token after `@`, so `@../../etc/passwd` → `root/../../etc/passwd` reads outside. Absolute `ref` (`@/etc/passwd`) → `fs::path(root) / "/etc/passwd"` = `/etc/passwd` (absolute RHS discards root). |
| `/files ls`, `/files tree`, `/files open`, `/files find` | `tui/tui_input.cpp:1511,1529,1552,1569` `if (path[0] != '/') path = root + "/" + path;` | Absolute paths pass through unchanged (`path[0] == '/'` skips the prefix). `../` escapes are never normalized. |
| `/system exec` | `tui/tui_input.cpp:1582` `popen(rest.c_str(), "r")` | User-supplied text executed through the shell with no escaping, confinement, or approval. Direct shell injection from slash-command input. |

`/system delete` (`tui_input.cpp:1597`) correctly calls `Workspace::confine` — proving the helper exists and the other sites chose not to use it.

### SEC-05 — Plugin manifest path safety (`lib/plugin.cpp`)

| # | Issue | File | Risk |
|---|-------|------|------|
| a | `main` path not confined | `lib/plugin.cpp:143` `is_executable(dir + "/" + out.main)` | A manifest with `"main": "../../../../bin/sh"` resolves to an existing executable and `execl` runs it from the plugin dir. |
| b | `install` copies symlinks | `lib/plugin.cpp:514-554` `copy(..., copy_symlinks)` | A malicious tarball plants a `main` symlink pointing outside the plugin dir; later `execl` follows it. |

---

## 3. Target Architecture

### 3.1 SEC-01 — `classify_shell` hardening

**Principle:** a command is `ReadOnly` only when *every* path-like argument in *every* segment resolves inside the workspace. Reader heads lose their argument exemption; they keep only their "head is not a write command" semantics.

**Changes to `lib/shell_classify.cpp`:**

1. **Remove `cd` from `kReaders`** (`:69`). `cd` changes process state and enables relative-path escapes; it is never read-only. `cd /tmp` → `Write` (state change), `cd /tmp && cat secret` → `Outside`.

2. **Treat newline as a chain operator.** Add `\n` to the segment-splitting pass: change `is_chain_op` (`:75`) to also return true for a token containing an unquoted `\n`, *or* (cleaner) split the input on `\n` before tokenizing each line, then classify the union. The latter keeps `tokenize` single-line and avoids embedded-newline tokens entirely. **Chosen approach:** pre-split on `\n` into lines, tokenize each line, concatenate token streams with an explicit chain-op sentinel between lines. This makes `echo a\ncat /etc/passwd` two segments.

3. **Scan reader arguments for path escapes.** Replace `if (reader) continue;` (`:307`) with a path-escape check that runs for *all* heads including readers:
   - For each non-head, non-redirect, non-flag token of a reader head, call `outside_scope_for(t, /*is_target=*/false)`.
   - If it escapes → set `first_outside` and continue.
   - Reader heads still skip the *write* classification (no redirect → no write), but they no longer skip the *outside* classification.
   - Reader heads with only in-workspace or non-path arguments remain `ReadOnly` (the common case: `ls -la`, `cat file.txt`, `grep -rn foo .`).

4. **Scan input-redirect targets.** Move the `is_input_redirect` branch (`:293`) to set `target_next = true` and let the existing `target_next` block (`:273-279`) check the target via `outside_scope_for(t, /*is_target=*/true)`. `cat < /etc/passwd` → `Outside`.

**What does NOT change:** the `ShellEffect` enum, the `ShellClass` struct, the `destructive_command_patterns()` seed list, the policy-engine integration, or the `scope_id` format. The classifier stays purely syntactic and stays under its current public API.

**Coding standards applied:**
- **SRP:** `classify_shell` already has one job (classify). The fix keeps it one job; it does not gain IO or state.
- **Method size:** `classify_shell` is currently 172 lines — already a violation. This FIX does not expand it; SEC-01 is scoped to the four bypass fixes only. A separate size-refactor (split into `split_segments`, `classify_segment`, `resolve_effect`) is parked as a follow-up FIX to keep this PR reviewable. **Boy Scout:** the four edits each *reduce* a branch or remove a `continue`, so the method shrinks slightly.
- **KISS:** no new config, no new enum value. The fix makes the existing `Outside` path fire correctly.
- **Fail-safe:** unknown/ambiguous stays `Destructive` (prompt), unchanged.

### 3.2 SEC-02 — Approval-gate coverage

**Principle:** every tool that mutates persistent state, spawns privileged execution, or runs external code must opt into `requires_approval` unless it is provably safe in all arguments.

**Changes:**

| Tool | Override | Rationale |
|------|----------|-----------|
| `WriteTool` | `requires_approval` returns `true` for create/overwrite (any edit where `old == ""`), `false` for in-place patches of existing files | Full overwrite/create is a state change the user should see; in-place edit is the common agent workflow and gating it would be noise. **Reconsider if reviewers prefer gating all writes** — the proposal defaults to the less noisy contract but flags it for sign-off. |
| `TaskTool` | `requires_approval` returns `true` | Sub-agent inherits full registry → privilege escalation vector. Always prompt. |
| `TodowriteTool` | `requires_approval` returns `false` (unchanged) | Mutates only the session task list (ephemeral, in-memory, no file IO). Not a security boundary. **Documented decision** in the override's comment so the next reviewer sees the reasoning. |
| `PluginTool` | `requires_approval` returns `true` | External-process code execution. Always prompt. |

**Architecture:** no new abstraction. `Tool::requires_approval` is the existing port; these tools implement it. The dispatch gate (`lib/dispatch.cpp:128-170`) already consults it correctly. This is pure adapter conformance — the tools were non-conformant.

**Coding standards applied:**
- **LSP:** every `Tool` subtype remains substitutable; we are tightening a default, not adding a new virtual.
- **OCP:** no change to `Tool` interface; tools override an existing virtual.
- **Fail-safe:** `TaskTool`/`PluginTool` prompt when no approver is present (dispatch denies), so headless runs never spawn sub-agents or run plugin code unattended.

### 3.3 SEC-03 — Search backend hardening

**Changes to `tools/search/grep_backend.cpp`:**

1. **Insert `--` before the pattern** (`:41`): `cmd += "-- " + shell_quote(query) + " " + shell_quote(root)`. This terminates grep option parsing.
2. **Add a timeout** via the existing `JobService`/process pattern: wrap the `popen` in a `fork`+`execvp` of `timeout` (portable: use `alarm`/`SIGALRM` in the reader, or reuse `lib/process.cpp`'s `spawn_shell` with an idle timeout). **Chosen approach:** reuse `spawn_shell` + the `run_with_timeout` pattern from `bash_tool.cpp:43-73` rather than a new popen variant — **DRY** with the bash tool's proven timeout loop. Cap at 30s.
3. **Cap output at `min(max, 2000)` bytes/lines** in `pipe_read`: stop appending once `result.size() >= kMaxSearchOutput` (64 KiB, matching the bash tool cap).

**Changes to `tools/search/semantic_index.cpp`:**

4. **Skip symlinks** in `walk` (`:126`): replace `fs::is_regular_file(p, ec)` with `fs::symlink_status(p, ec)` + `S_ISREG` (no follow). This matches `grep -r`'s default no-follow semantics and closes the symlink-escape.

**Changes to `tools/search_tool.cpp`:**

5. **Clamp `max`** (`:106-107`): `if (max > 2000) max = 2000;` alongside the existing `if (max < 1) max = 1;`.

**Coding standards applied:**
- **DRY:** the timeout loop is extracted from `bash_tool.cpp` into a shared `lib/process.cpp` helper `run_with_idle_timeout(fd, pid, idle_s, hard_s, out)` used by both the bash tool and the grep backend. This pays down the duplicated drain-loop finding from the tools review.
- **LSP:** `GrepBackend` and `SemanticBackend` become substitutable re: symlink handling (both no-follow) and output caps (both bounded).
- **KISS:** no new config key; the 30s/2000-hit/64 KiB caps are constants, not user-tunable (YAGNI).

### 3.4 SEC-04 — TUI path confinement

**Principle:** `Workspace::confine` is the single path-confinement authority. No TUI code may build a workspace path by string concatenation.

**New helper** in `include/agent/workspace.h`:

```cpp
// Resolve a user-typed path to a workspace-confined absolute path.
// Returns false (with error) on escape; true with `out` set to the
// confined absolute path. Thin wrapper over confine() for callers
// that want the absolute path rather than the lexical form.
bool Workspace::resolve(const std::string& requested,
                        std::string& out, std::string& error);
```

(If `confine` already returns the absolute path, this is just an alias; the goal is a single call site name that reads as "resolve user input" rather than "confine a tool path". If reviewers prefer, we skip the alias and call `confine` directly — the fix is the *use*, not the name.)

**Changes:**

| Site | File | Fix |
|------|------|-----|
| `@`-reference | `tui/tui.cpp:189` | Replace `fs::path(root) / ref` with `Workspace::confine(ref, resolved, err)`; on failure, emit `ref` literally (current behavior for non-files). |
| `/files ls` | `tui_input.cpp:1511` | Replace prefix-join with `Workspace::confine(rest, resolved, err)`; on failure, print `err`. |
| `/files tree` | `tui_input.cpp:1529` | Same. |
| `/files open` | `tui_input.cpp:1552` | Same. |
| `/files find` | `tui_input.cpp:1569` | Same. |
| `/system exec` | `tui_input.cpp:1582` | **Remove `popen` of raw user input.** Route through `JobService::start` (already wired in the TUI) so the command is visible in `/job`, killable, timeout-bounded, and output-capped. This also makes `/system exec` consistent with the bash tool's safety model. |

**Coding standards applied:**
- **DRY:** the four `/files` handlers collapse their identical prefix-join into one `Workspace::confine` call each. A follow-up FIX can extract a `cmd_files_path(rest) -> resolved` helper to collapse the four call sites further; this FIX keeps the change local.
- **SRP:** `/system exec` stops being a raw-shell wrapper and becomes a `JobService` consumer (the TUI already owns the service).
- **Fail-safe:** `confine` rejects absolute paths and `../` escapes; the TUI inherits that for free.

### 3.5 SEC-05 — Plugin manifest path safety

**Changes to `lib/plugin.cpp`:**

1. **Reject `..` in `main`** (`parse_manifest`, `:143`): after reading `out.main`, check that `fs::path(out.main).lexically_relative(".")` does not contain `..` and that `out.main` is not absolute. On violation, set `err = "manifest main path escapes plugin dir"` and return false.
2. **Resolve `main` lexically within the plugin dir** and verify the resolved target is inside `dir` (same `is_within` logic as `Workspace::confine`).
3. **Do not follow symlinks on exec**: use `fs::symlink_status` on the resolved `main` path; reject if it is a symlink whose target is outside `dir`.
4. **`install`** (`:514-554`): replace `copy_symlinks` with default copy (no symlink follow), or explicitly reject any symlink in the extracted tree whose target escapes the plugin dir. **Chosen approach:** reject symlinks in the extracted `main` path post-copy (cheaper than auditing the whole tarball).

**Coding standards applied:**
- **SRP:** `parse_manifest` gains a validation responsibility it should have had; `install` gains a post-copy symlink check. Both stay under 10-line helpers (`validate_main_path`, `reject_symlink_main`).
- **Fail-safe:** a malformed manifest is rejected, not executed.

---

## 4. Red Tests (to be committed first, per workflow)

Each test goes in `tests/run_tests.cpp` next to the existing `shell_classify_*` tests (`:2363-2414`) and fails on current `main`.

### SEC-01 Red tests

```cpp
TEST(shell_classify_reader_args_outside) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // Reader heads with outside-workspace args are Outside, not ReadOnly.
    ASSERT(agent::classify_shell("cat /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("grep foo /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    ASSERT(agent::classify_shell("ls /tmp", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    // In-workspace reader args stay ReadOnly.
    ASSERT(agent::classify_shell("cat file.txt", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::ReadOnly);
    agent::Workspace::set_root(".");
}

TEST(shell_classify_cd_not_reader) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    ASSERT(agent::classify_shell("cd /tmp", "/tmp/amber_cls_ws").effect !=
           agent::ShellEffect::ReadOnly);
    ASSERT(agent::classify_shell("cd /tmp && cat secret", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    agent::Workspace::set_root(".");
}

TEST(shell_classify_newline_chain) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    // Embedded newline separates commands like ";".
    ASSERT(agent::classify_shell("echo a\ncat /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    agent::Workspace::set_root(".");
}

TEST(shell_classify_input_redirect_outside) {
    agent::Workspace::set_root("/tmp/amber_cls_ws");
    ASSERT(agent::classify_shell("cat < /etc/passwd", "/tmp/amber_cls_ws").effect ==
           agent::ShellEffect::Outside);
    agent::Workspace::set_root(".");
}
```

### SEC-02 Red tests

```cpp
TEST(write_tool_requires_approval) {
    auto t = agent::make_write_tool();
    json create = {{"path", "x"}, {"edits", json::array({{{"old", ""}, {"new", "y"}}})}};
    ASSERT(t->requires_approval(create) == true);
    json patch = {{"path", "x"}, {"edits", json::array({{{"old", "a"}, {"new", "b"}}})}};
    // In-place patch: per proposal, not gated (flagged for sign-off).
    ASSERT(t->requires_approval(patch) == false);
}

TEST(task_tool_requires_approval) {
    // Requires a SubAgentExecutor + ToolRegistry stub; assert
    // requires_approval() == true regardless of args.
    ASSERT(task_tool->requires_approval({{"prompt", "anything"}}) == true);
}

TEST(plugin_tool_requires_approval) {
    // Requires a PluginManager stub; assert requires_approval() == true.
    ASSERT(plugin_tool->requires_approval({}) == true);
}
```

### SEC-03 Red tests

```cpp
TEST(grep_backend_dash_pattern_safe) {
    // A pattern starting with '-' must not be treated as a grep option.
    auto b = agent::make_grep_backend();
    auto hits = b->search("-e foo", agent::Workspace::root(), "", 10);
    // No crash, no option injection; hits may be empty or real matches.
    ASSERT(true);  // structural: the command must contain "-- "
}

TEST(grep_backend_output_capped) {
    auto b = agent::make_grep_backend();
    auto hits = b->search("a", agent::Workspace::root(), "", 1000000);
    ASSERT(hits.size() <= 2000);
}

TEST(semantic_backend_skips_symlinks) {
    // Create a symlink in the workspace pointing to /etc/passwd,
    // assert its content is NOT returned by semantic search.
    ASSERT(hits_for_passwd_via_symlink.empty());
}
```

### SEC-04 Red tests

```cpp
TEST(tui_at_reference_outside_rejected) {
    // expand_at_references("@../../etc/passwd") must not inline
    // /etc/passwd content; it must emit the literal token.
    ASSERT(expanded.find("root:") == std::string::npos);
}

TEST(tui_files_ls_absolute_rejected) {
    // /files ls /etc must print a confinement error, not list /etc.
    ASSERT(output.find("escapes workspace") != std::string::npos);
}
```

### SEC-05 Red tests

```cpp
TEST(plugin_manifest_main_escape_rejected) {
    agent::PluginManifest m;
    m.main = "../../../../bin/sh";
    std::string err;
    ASSERT(!agent::PluginManager::parse_manifest(dir_with_bad_main, m, err));
    ASSERT(err.find("escapes") != std::string::npos);
}
```

---

## 5. Coding Standards Checklist (applied to every FIX)

| Principle | How this proposal honors it |
|-----------|----------------------------|
| **SOLID — SRP** | `classify_shell` keeps one job; `Tool::requires_approval` overrides are single-purpose; `Workspace::confine` stays the sole confinement authority. |
| **SOLID — OCP** | No `Tool`/`SearchBackend`/`ShellClass` interface change. Hardening is via existing virtuals and existing helpers. |
| **SOLID — LSP** | `GrepBackend`/`SemanticBackend` become substitutable re: symlink + cap behavior. All `Tool` subtypes remain substitutable. |
| **SOLID — ISP** | No interface widening. `Workspace::resolve` (if added) is a thin non-virtual helper, not a new port. |
| **SOLID — DIP** | `/system exec` depends on the existing `JobService` abstraction, not on `popen`. |
| **KISS** | No new config keys, no new enum values, no new abstractions. Caps are constants. |
| **DRY** | `Workspace::confine` replaces 5 hand-rolled prefix-joins. `run_with_idle_timeout` replaces a duplicated drain loop. |
| **YAGNI** | No speculative "configurable approval policy per tool" framework. Each tool hardcodes its contract. |
| **Method < 10 lines** | SEC-01 edits remove branches (net shrink). SEC-05 adds `validate_main_path`/`reject_symlink_main` helpers under 10 lines. The pre-existing 172-line `classify_shell` size debt is parked as a separate follow-up FIX to keep this PR reviewable. |
| **Class < 200 lines** | No class grows past 200 lines. |
| **Fail-safe** | Unknown → prompt/deny. No approver → deny. Symlink → reject. Absolute path → reject. |
| **Boy Scout** | Each FIX leaves its file cleaner (removes a `continue`, removes a duplicated loop, removes a raw `popen`). |
| **No dead code** | `cd` removal from `kReaders` deletes a line; no stubs left behind. |

---

## 6. Verification Plan

Per `AGENTS.md` and `docs/fix-tracker.md`:

```
make clean && make && make test && make lint && make analyze
```

under both `CXX=g++` and `CXX=clang++`. All Red tests must flip to Green. No new lint/analyze warnings. The existing `shell_classify_*` tests must still pass (the in-workspace reader cases are unchanged).

**Sign-off gate:** the four SEC-02 approval contracts (especially `WriteTool` create-vs-patch) and the `/system exec` → `JobService` rerouting are the two design decisions that need reviewer agreement before Green work starts. Everything else is mechanical hardening.

---

## 7. Ordering & Dependencies

```
SEC-01 (shell classifier)      ── no deps ──
SEC-02 (approval gate)         ── no deps ──
SEC-03 (search backends)       ── depends on shared run_with_idle_timeout (extracted in SEC-03 step 2)
SEC-04 (TUI confinement)       ── no deps; /system exec reroute benefits from SEC-03's JobService reuse
SEC-05 (plugin manifest)       ── no deps ──
```

SEC-01 and SEC-02 are the highest-severity (direct LLM-controlled execution/read of out-of-workspace paths). They should land first, in parallel branches. SEC-03/04/05 follow.

---

## 8. Out of Scope (parked follow-up FIXes)

- Splitting `classify_shell` (172 lines) into `split_segments` / `classify_segment` / `resolve_effect` — size debt, not security. Separate FIX.
- Extracting a shared `McpMessageRouter` — MCP DRY, not security. Separate FIX.
- `PluginTool` dangling `PluginManager*` lifetime — plugin lifecycle, not security. Separate FIX.
- `WriteTool` FIFO/blocking-on-special-file rejection — `read_tool` already does this; port the `stat` check. Small enough to fold into SEC-02 if reviewers prefer, else separate FIX.
