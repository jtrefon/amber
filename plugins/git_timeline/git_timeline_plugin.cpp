#include "git_timeline_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

namespace {

// The body is split across three sections because the complexity gate counts
// a multi-line string literal as lines of code: one 99-line function is both a
// gate failure and harder to edit than three sections with one job each.
std::string snapshot_section() {
    return R"(# Git timeline: snapshots you can find again

A commit is the unit of work here. Its subject is the index you scan weeks
later, so the message is part of the change, not a label stuck on it.

## The snapshot step

Take one snapshot when a step is complete, before the reply that ends the turn:

    git add <the paths you changed>
    git commit -m "<area>: <what changed>"
    git log --oneline -1
    git status --short

The last two commands are the check: the commit exists, and the tree is clean.
A tree with changes left at the end of the turn is work the timeline did not
capture. An empty commit is a sign the step is not done; name the paths you
mean, so unrelated work (and a stray credential) stays out of the snapshot.

## The index key

    <area>: <imperative summary>          <= 72 characters

One area per commit. Two unrelated areas are two commits: a key that covers
everything indexes nothing. The area comes from the project -- its AGENTS.md,
its own paths, the vocabulary already in `git log`. `tui:`, `lib:`, `docs:`,
`plugins:` are examples, not a fixed list. The body carries the why, and what
reverting the commit would undo; one line is enough when the subject says it.

WIP and fixup subjects are working notes, not index entries: squash them before
a branch is offered for review (`git rebase -i --autosquash <base>`).

The snapshot is one completed step, not one edit and not the session. Group
edits that answer one question; split a change nobody could review in one
`git show --stat`.

)";
}

std::string history_section() {
    return R"(## Scanning the index

    git log --oneline --no-merges -50          # the index, newest first
    git log --oneline --grep '^tui:' -20       # one area
    git log --oneline --since=2026-10-01 --until=2026-10-07
    git show --stat <sha>                      # what a key stands for
    git log --oneline -- path/to/file          # one path's history

Start from the key, then narrow by path, author or date. When the log is long,
`--grep` on the area is cheaper than reading all of it.

## Pinpointing a change

    git log -S'<string>' -- <path>     # when a string appeared or disappeared
    git log -L :<function>:<file>      # one function's history
    git blame -L <start>,<end> <file>  # which snapshot owns these lines
    git diff <old>..<new> -- <path>    # the delta between two points

## Temporal search: which snapshot broke it

    git bisect start
    git bisect bad <known-bad>
    git bisect good <known-good>
    git bisect run ./your-check        # automated; stops at the first bad commit
    git bisect reset

The index makes this cheap: the bad commit's subject names the change to read
first. A bisect run inside a worktree leaves the tree you are working in alone.

)";
}

std::string rollback_section() {
    return R"(## Rolling back

Inspect first -- `git show <sha>`, `git diff <sha>~1 <sha>` -- then choose by
what the history is shared with:

| State | Route | Why |
|---|---|---|
| Uncommitted edits | `git restore <paths>` (or `git stash`) | nothing to rewrite |
| Local snapshot, unpushed | `git reset --soft <sha>` (or `--mixed`) | keeps the work, drops the commit |
| Pushed, shared, or the project's mainline | `git revert <sha>` | an undo commit; history others built on stays intact |

`git reset --hard` discards uncommitted work, so it is the user's call, not a
step taken on your own; rewriting pushed history is the same kind of decision.
When a revert conflicts, stop and report the conflict: the resolution is a
judgement about the other change, not a mechanical step.

## Worktrees

An amber window workspace is a detached worktree: create a branch before the
first commit (`git switch -c fix/<topic>`), and the window will not discard
commits no branch reaches. A WIP commit on that branch is safer than a stash:
the stash is shared by every worktree of the repository, so a sibling window
can pop it.

## Recovering a lost snapshot

    git reflog                 # every HEAD move, including reset and rebase
    git fsck --lost-found      # dangling commits, content kept without a name

A commit that exists is reachable by hash even when no branch names it: the
reflog is the first place to look, the object store the second.
)";
}

} // namespace

std::string render_git_timeline_skill() {
    return snapshot_section() + history_section() + rollback_section();
}

std::vector<std::unique_ptr<Capability>> GitTimelinePlugin::capabilities() {
    SystemSkill skill;
    skill.name = "git-timeline";
    skill.description =
        "Snapshots with an indexed subject, and how to scan, pinpoint and undo them";
    skill.body = [] { return render_git_timeline_skill(); };
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<SkillCapability>(std::move(skill)));
    return caps;
}

} // namespace agent::plugins
