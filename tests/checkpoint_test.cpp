// Per-session checkpoints: the commit on refs/amber/checkpoints/<session>, the
// structured message that acts as the index, the shadow repo for a project that
// is not a git checkout, and the promise that the user's HEAD and index never
// move.
//
// The store is exercised against real temporary repositories; no git daemon and
// no network.

#include "agent/checkpoint.h"
#include "agent/checkpoint_format.h"
#include "agent/checkpoint_service.h"
#include "agent/checkpoint_turn.h"
#include "agent/config.h"
#include "agent/git_exec.h"
#include "agent/run_scope.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "plugins/tool_checkpoint/tool_checkpoint_plugin.h"
#include "tests/test_util.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace agent {

// ASSERT_EQ prints both operands; the enum has no production stream operator.
inline std::ostream& operator<<(std::ostream& os, CheckpointChangeKind kind) {
    return os << static_cast<int>(kind);
}

} // namespace agent

namespace {

namespace fs = std::filesystem;

std::string trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

class TempProject {
public:
    TempProject(const std::string& tag, bool git)
        : dir_(fs::temp_directory_path() / ("amber_cp_" + tag)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        if (!git)
            return;
        git_ok({"init", "-q"});
        git_ok({"config", "user.email", "checkpoint@example.com"});
        git_ok({"config", "user.name", "Checkpoint Test"});
    }

    ~TempProject() { fs::remove_all(dir_); }

    const fs::path& path() const { return dir_; }
    std::string str() const { return dir_.string(); }

    void write(const std::string& name, const std::string& content) const {
        const fs::path target = dir_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream out(target);
        out << content;
    }

    std::string read(const std::string& name) const {
        std::ifstream in(dir_ / name);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    bool exists(const std::string& name) const { return fs::exists(dir_ / name); }

    agent::GitResult git(const std::vector<std::string>& args) const {
        return agent::run_git(dir_.string(), args);
    }

    void commit_all(const std::string& message) const {
        git_ok({"add", "-A"});
        git_ok({"commit", "-q", "-m", message});
    }

private:
    void git_ok(const std::vector<std::string>& args) const {
        const agent::GitResult result = git(args);
        ASSERT_EQ(result.status, 0);
    }

    fs::path dir_;
};

} // namespace

// The first checkpoint snapshots the whole worktree (there is no parent
// checkpoint); the next one is a child of it. Both write a commit on the
// session ref and a message the agent can parse.
TEST(checkpoint_create_writes_the_ref_and_the_structured_message) {
    agent::test::XdgGuard xdg("checkpoint_create");
    TempProject project("create", /*git=*/true);
    project.write("a.txt", "one\n");
    project.commit_all("init");

    agent::CheckpointStore store(project.str(), "sess-a");
    ASSERT_FALSE(store.shadow());
    ASSERT_EQ(store.ref(), std::string("refs/amber/checkpoints/sess-a"));

    std::string error;
    const auto first = store.create(1, "add a file", error);
    ASSERT(first.has_value());
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(first->turn, 1u);
    ASSERT_EQ(first->prompt, std::string("add a file"));
    ASSERT_EQ(first->files, 1u); // the whole tree on the first checkpoint

    const agent::GitResult ref = project.git({"rev-parse", store.ref()});
    ASSERT_EQ(ref.status, 0);
    ASSERT_EQ(trimmed(ref.output), first->id);

    const agent::GitResult message = project.git({"show", "-s", "--format=%B", first->id});
    ASSERT_EQ(message.status, 0);
    ASSERT(message.output.find("amber: turn 1") != std::string::npos);
    ASSERT(message.output.find("Turn: 1 | Session: sess-a | Files: 1 changed") !=
           std::string::npos);
    ASSERT(message.output.find("Prompt: \"add a file\"") != std::string::npos);

    project.write("b.txt", "two\n");
    const auto second = store.create(2, "add b", error);
    ASSERT(second.has_value());
    ASSERT_EQ(second->files, 1u); // one file changed since the first checkpoint

    const agent::GitResult parent = project.git({"rev-parse", second->id + "^"});
    ASSERT_EQ(parent.status, 0);
    ASSERT_EQ(trimmed(parent.output), first->id);
}

// A turn that changed nothing does not create a commit: the tree is identical
// to the previous checkpoint and the index is the only thing that moved.
TEST(checkpoint_skips_a_read_only_turn) {
    agent::test::XdgGuard xdg("checkpoint_read_only");
    TempProject project("read_only", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-ro");
    std::string error;
    const auto first = store.create(1, "first", error);
    ASSERT(first.has_value());

    const auto second = store.create(2, "read only", error);
    ASSERT_FALSE(second.has_value());
    ASSERT_TRUE(error.empty());

    const auto list = store.list(error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list.front().turn, 1u);
}

// The user's work is theirs: a checkpoint stages through a private index and
// commits on its own ref, never touching HEAD or the real index.
TEST(checkpoint_never_moves_head_or_the_user_index) {
    agent::test::XdgGuard xdg("checkpoint_head");
    TempProject project("head", /*git=*/true);
    project.write("a.txt", "one\n");
    project.commit_all("init");
    project.write("a.txt", "two\n");
    const agent::GitResult staged = project.git({"add", "a.txt"});
    ASSERT_EQ(staged.status, 0);

    const std::string head_before = trimmed(project.git({"rev-parse", "HEAD"}).output);
    const std::string status_before = project.git({"status", "--porcelain"}).output;

    agent::CheckpointStore store(project.str(), "sess-head");
    std::string error;
    const auto checkpoint = store.create(1, "change a", error);
    ASSERT(checkpoint.has_value());
    ASSERT_TRUE(error.empty());

    ASSERT_EQ(trimmed(project.git({"rev-parse", "HEAD"}).output), head_before);
    ASSERT_EQ(project.git({"status", "--porcelain"}).output, status_before);
    const agent::GitResult cached = project.git({"diff", "--cached", "--name-only"});
    ASSERT(cached.output.find("a.txt") != std::string::npos);
}

// A project that is not a git checkout gets a private bare repository under the
// state directory, with the project as its worktree. Nothing appears in the
// project, so there is no .gitignore to edit and no .git to confuse a later
// `git init`.
TEST(checkpoint_uses_a_shadow_repo_outside_the_project) {
    agent::test::XdgGuard xdg("checkpoint_shadow");
    TempProject project("shadow", /*git=*/false);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-shadow");
    ASSERT(store.shadow());

    std::string error;
    const auto checkpoint = store.create(1, "first", error);
    ASSERT(checkpoint.has_value());
    ASSERT_TRUE(error.empty());
    ASSERT_FALSE(fs::exists(project.path() / ".git"));

    const auto list = store.list(error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list.front().id, checkpoint->id);
}

// Newest first, by turn or by SHA prefix; an unknown selector is not an error.
TEST(checkpoint_list_is_newest_first_and_find_by_turn_or_sha) {
    agent::test::XdgGuard xdg("checkpoint_list");
    TempProject project("list", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-list");
    std::string error;
    ASSERT(store.create(1, "one", error).has_value());
    project.write("b.txt", "two\n");
    ASSERT(store.create(2, "two", error).has_value());
    project.write("c.txt", "three\n");
    ASSERT(store.create(3, "three", error).has_value());

    const auto list = store.list(error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(list.size(), 3u);
    ASSERT_EQ(list[0].turn, 3u);
    ASSERT_EQ(list[1].turn, 2u);
    ASSERT_EQ(list[2].turn, 1u);

    const auto by_turn = store.find("2", error);
    ASSERT(by_turn.has_value());
    ASSERT_EQ(by_turn->turn, 2u);
    ASSERT_EQ(by_turn->prompt, std::string("two"));

    // An id prefix is only a SHA selector while it carries a hex letter: an
    // all-digit string is a turn number, never a SHA prefix.
    std::string sha_prefix;
    std::string sha_owner;
    for (const auto& checkpoint : list) {
        const std::size_t letter = checkpoint.id.find_first_of("abcdef");
        if (letter != std::string::npos) {
            sha_prefix = checkpoint.id.substr(0, letter + 1);
            sha_owner = checkpoint.id;
            break;
        }
    }
    ASSERT_FALSE(sha_prefix.empty());
    const auto by_sha = store.find(sha_prefix, error);
    ASSERT(by_sha.has_value());
    ASSERT_EQ(by_sha->id, sha_owner);

    ASSERT_FALSE(store.find("99", error).has_value());
    ASSERT_TRUE(error.empty());

    // The same rule holds when a random checkpoint id starts with the digits:
    // measure the collision instead of relying on the hashes not to contain
    // one. A 3-digit prefix is > any turn this test created, so only the old
    // SHA fall-through could answer it.
    std::string numeric_prefix;
    std::size_t turn = 3;
    for (std::size_t extra = 0; extra < 40 && numeric_prefix.empty(); ++extra) {
        ++turn;
        project.write("filler.txt", "filler " + std::to_string(turn) + "\n");
        ASSERT_TRUE(store.create(turn, "filler " + std::to_string(turn), error).has_value());
        for (const auto& checkpoint : store.list(error)) {
            if (checkpoint.id.size() < 3 || !std::isdigit(checkpoint.id[0]) ||
                !std::isdigit(checkpoint.id[1]) || !std::isdigit(checkpoint.id[2]))
                continue;
            const unsigned value =
                static_cast<unsigned>((checkpoint.id[0] - '0') * 100 +
                                      (checkpoint.id[1] - '0') * 10 + (checkpoint.id[2] - '0'));
            if (value > turn) {
                numeric_prefix = checkpoint.id.substr(0, 3);
                break;
            }
        }
    }
    ASSERT_FALSE(numeric_prefix.empty());
    if (!numeric_prefix.empty()) {
        ASSERT_FALSE(store.find(numeric_prefix, error).has_value());
        ASSERT_TRUE(error.empty());
    }
}

// The diff a checkpoint introduced, against its parent; the first checkpoint is
// against the empty tree.
TEST(checkpoint_diff_shows_the_turn_change) {
    agent::test::XdgGuard xdg("checkpoint_diff");
    TempProject project("diff", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-diff");
    std::string error;
    ASSERT(store.create(1, "one", error).has_value());
    project.write("a.txt", "two\n");
    ASSERT(store.create(2, "two", error).has_value());

    const auto list = store.list(error);
    ASSERT_EQ(list.size(), 2u);
    const std::string first = store.diff(list[1], error);
    ASSERT_TRUE(error.empty());
    ASSERT(first.find("+one") != std::string::npos);

    const std::string second = store.diff(list[0], error);
    ASSERT_TRUE(error.empty());
    ASSERT(second.find("-one") != std::string::npos);
    ASSERT(second.find("+two") != std::string::npos);
}

// Two sessions on one project have independent histories, which is what keeps a
// window's undo stack its own.
TEST(checkpoint_sessions_are_isolated) {
    agent::test::XdgGuard xdg("checkpoint_sessions");
    TempProject project("sessions", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore first(project.str(), "session-one");
    agent::CheckpointStore second(project.str(), "session-two");
    std::string error;
    ASSERT(first.create(1, "one", error).has_value());
    ASSERT(second.create(1, "two", error).has_value());

    ASSERT(first.ref() != second.ref());
    ASSERT_EQ(first.list(error).size(), 1u);
    ASSERT_EQ(second.list(error).size(), 1u);
    ASSERT_EQ(first.list(error).front().prompt, std::string("one"));
    ASSERT_EQ(second.list(error).front().prompt, std::string("two"));
}

// Ignored files are not part of a checkpoint: the private index respects the
// project's .gitignore exactly as a normal `git add -A` would.
TEST(checkpoint_ignores_ignored_files) {
    agent::test::XdgGuard xdg("checkpoint_ignored");
    TempProject project("ignored", /*git=*/true);
    project.write(".gitignore", "ignored.txt\n");
    project.write("tracked.txt", "tracked\n");
    project.write("ignored.txt", "ignored\n");

    agent::CheckpointStore store(project.str(), "sess-ignored");
    std::string error;
    const auto checkpoint = store.create(1, "first", error);
    ASSERT(checkpoint.has_value());

    const agent::GitResult tree = project.git({"ls-tree", "-r", "--name-only", checkpoint->id});
    ASSERT_EQ(tree.status, 0);
    ASSERT(tree.output.find("tracked.txt") != std::string::npos);
    ASSERT(tree.output.find("ignored.txt") == std::string::npos);
}

// The message stays one line per field: a quit-and-reload can parse it without
// guessing where the prompt ended.
TEST(checkpoint_escapes_a_multiline_prompt) {
    agent::test::XdgGuard xdg("checkpoint_escape");
    TempProject project("escape", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-escape");
    std::string error;
    const auto checkpoint = store.create(1, "line one\nline \"two\"", error);
    ASSERT(checkpoint.has_value());
    ASSERT_EQ(checkpoint->prompt, std::string("line one\nline \"two\""));

    const agent::GitResult message = project.git({"show", "-s", "--format=%B", checkpoint->id});
    ASSERT(message.output.find("Prompt: \"line one\\nline \\\"two\\\"\"") != std::string::npos);
}

namespace {

// A PATH shim that delegates to the real git unless an argument contains the
// injected failure word. It is how the store's error branches are exercised
// without a corrupt repository.
class FakeGit {
public:
    explicit FakeGit(const std::string& fail_arg)
        : dir_(fs::temp_directory_path() / "amber_fake_git") {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        const std::string real = resolve_git();
        const fs::path script = dir_ / "git";
        std::ofstream out(script);
        out << "#!/bin/sh\n";
        out << "if [ \"$1\" != \"-C\" ]; then exec \"" << real << "\" \"$@\"; fi\n";
        out << "repo=\"$2\"; shift 2\n";
        out << "for arg in \"$@\"; do\n";
        out << "  case \"$arg\" in *\"" << fail_arg
            << "\"*) echo \"injected failure\" >&2; exit 1;; esac\n";
        out << "done\n";
        out << "exec \"" << real << "\" -C \"$repo\" \"$@\"\n";
        out.close();
        fs::permissions(script, fs::perms::owner_all | fs::perms::group_read |
                                    fs::perms::group_exec | fs::perms::others_read |
                                    fs::perms::others_exec);
        const char* path = std::getenv("PATH");
        old_path_ = path ? path : "";
        setenv("PATH", (dir_.string() + ":" + old_path_).c_str(), 1);
    }

    ~FakeGit() {
        setenv("PATH", old_path_.c_str(), 1);
        fs::remove_all(dir_);
    }

    FakeGit(const FakeGit&) = delete;
    FakeGit& operator=(const FakeGit&) = delete;

private:
    static std::string resolve_git() {
        const char* path = std::getenv("PATH");
        std::istringstream in(path ? path : "");
        std::string part;
        while (std::getline(in, part, ':')) {
            if (part.empty())
                continue;
            const fs::path candidate = fs::path(part) / "git";
            std::error_code ec;
            if (fs::exists(candidate, ec))
                return candidate.string();
        }
        return "/usr/bin/git";
    }

    fs::path dir_;
    std::string old_path_;
};

class PathGuard {
public:
    explicit PathGuard(const std::string& path) {
        const char* old = std::getenv("PATH");
        old_ = old ? old : "";
        setenv("PATH", path.c_str(), 1);
    }
    ~PathGuard() { setenv("PATH", old_.c_str(), 1); }

private:
    std::string old_;
};

} // namespace

// A project with no files has nothing to checkpoint; the first snapshot is not
// a commit of the empty tree.
TEST(checkpoint_empty_project_creates_nothing) {
    agent::test::XdgGuard xdg("checkpoint_empty");
    TempProject project("empty", /*git=*/true);

    agent::CheckpointStore store(project.str(), "sess-empty");
    std::string error;
    ASSERT_FALSE(store.create(1, "nothing", error).has_value());
    ASSERT_TRUE(error.empty());
}

TEST(checkpoint_find_rejects_an_empty_selector) {
    agent::test::XdgGuard xdg("checkpoint_empty_selector");
    TempProject project("empty_selector", /*git=*/true);
    project.write("a.txt", "one\n");
    agent::CheckpointStore store(project.str(), "sess-find");
    std::string error;
    ASSERT(store.create(1, "one", error).has_value());
    ASSERT_FALSE(store.find("", error).has_value());
    ASSERT_TRUE(error.empty());
}

// The session id is user-facing and may contain anything; the ref component it
// becomes is safe, and two ids that sanitize the same still address one ref.
TEST(checkpoint_session_id_is_made_ref_safe) {
    agent::test::XdgGuard xdg("checkpoint_session_safe");
    TempProject project("session_safe", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "my session/with .. dots");
    ASSERT(store.ref().find(' ') == std::string::npos);
    ASSERT(store.ref().find("..") == std::string::npos);
    std::string error;
    ASSERT(store.create(1, "one", error).has_value());
    ASSERT_EQ(store.list(error).size(), 1u);
}

// The prompt survives the message exactly, including quotes, backslashes,
// control characters and non-ASCII text; only the subject is flattened.
TEST(checkpoint_prompt_round_trips_through_the_message) {
    agent::test::XdgGuard xdg("checkpoint_prompt");
    TempProject project("prompt", /*git=*/true);
    project.write("a.txt", "one\n");
    const std::string prompt = "quote \" backslash \\ newline\nreturn\rtab\tcaf\u00e9";

    agent::CheckpointStore store(project.str(), "sess-prompt");
    std::string error;
    ASSERT(store.create(1, prompt, error).has_value());
    const auto list = store.list(error);
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list.front().prompt, prompt);
}

// A fresh shadow history is empty, not an error: no repository has to exist
// until the first checkpoint.
TEST(checkpoint_fresh_shadow_lists_empty) {
    agent::test::XdgGuard xdg("checkpoint_fresh_shadow");
    TempProject project("fresh_shadow", /*git=*/false);
    agent::CheckpointStore store(project.str(), "sess-fresh");
    ASSERT(store.shadow());
    std::string error;
    ASSERT(store.list(error).empty());
    ASSERT_TRUE(error.empty());
}

// A diff for an id this repository never wrote fails cleanly.
TEST(checkpoint_diff_rejects_an_unknown_id) {
    agent::test::XdgGuard xdg("checkpoint_diff_unknown");
    TempProject project("diff_unknown", /*git=*/true);
    project.write("a.txt", "one\n");
    agent::CheckpointStore store(project.str(), "sess-diff-unknown");
    agent::Checkpoint bogus;
    bogus.id = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
    std::string error;
    ASSERT(store.diff(bogus, error).empty());
    ASSERT_FALSE(error.empty());
}

TEST(checkpoint_reports_a_failed_stage) {
    agent::test::XdgGuard xdg("checkpoint_fail_stage");
    TempProject project("fail_stage", /*git=*/true);
    project.write("a.txt", "one\n");
    FakeGit fake("add");
    agent::CheckpointStore store(project.str(), "sess-fail-stage");
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not stage") != std::string::npos);
}

TEST(checkpoint_reports_a_failed_tree_write) {
    agent::test::XdgGuard xdg("checkpoint_fail_tree");
    TempProject project("fail_tree", /*git=*/true);
    project.write("a.txt", "one\n");
    FakeGit fake("write-tree");
    agent::CheckpointStore store(project.str(), "sess-fail-tree");
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not write the checkpoint tree") != std::string::npos);
}

TEST(checkpoint_reports_a_failed_listing) {
    agent::test::XdgGuard xdg("checkpoint_fail_listing");
    TempProject project("fail_listing", /*git=*/true);
    project.write("a.txt", "one\n");
    FakeGit fake("ls-tree");
    agent::CheckpointStore store(project.str(), "sess-fail-listing");
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not list") != std::string::npos);
}

TEST(checkpoint_reports_a_failed_commit) {
    agent::test::XdgGuard xdg("checkpoint_fail_commit");
    TempProject project("fail_commit", /*git=*/true);
    project.write("a.txt", "one\n");
    FakeGit fake("commit-tree");
    agent::CheckpointStore store(project.str(), "sess-fail-commit");
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not write the checkpoint commit") != std::string::npos);
}

TEST(checkpoint_reports_a_failed_ref_update) {
    agent::test::XdgGuard xdg("checkpoint_fail_ref");
    TempProject project("fail_ref", /*git=*/true);
    project.write("a.txt", "one\n");
    FakeGit fake("update-ref");
    agent::CheckpointStore store(project.str(), "sess-fail-ref");
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not update") != std::string::npos);
}

TEST(checkpoint_reports_a_failed_diff) {
    agent::test::XdgGuard xdg("checkpoint_fail_diff");
    TempProject project("fail_diff", /*git=*/true);
    project.write("a.txt", "one\n");
    agent::CheckpointStore store(project.str(), "sess-fail-diff");
    std::string error;
    const auto checkpoint = store.create(1, "one", error);
    ASSERT(checkpoint.has_value());

    FakeGit fake("diff");
    ASSERT(store.diff(*checkpoint, error).empty());
    ASSERT(error.find("could not diff") != std::string::npos);
}

// Without git at all, the store degrades to a shadow store that reports the
// failure where a snapshot was asked for and an empty history otherwise.
TEST(checkpoint_reports_missing_git) {
    agent::test::XdgGuard xdg("checkpoint_no_git");
    TempProject project("no_git", /*git=*/false);
    project.write("a.txt", "one\n");
    PathGuard no_git("/nonexistent");

    agent::CheckpointStore store(project.str(), "sess-no-git");
    ASSERT(store.shadow());
    std::string error;
    ASSERT_FALSE(store.create(1, "one", error).has_value());
    ASSERT(error.find("could not create the checkpoint repository") != std::string::npos);

    ASSERT(store.list(error).empty());
    ASSERT_TRUE(error.empty());

    agent::Checkpoint bogus;
    bogus.id = "deadbeef";
    ASSERT(store.diff(bogus, error).empty());
    ASSERT_FALSE(error.empty());
}

// A restore puts the working tree back to the snapshot without touching HEAD
// or the real index. Files the snapshot did not contain are removed; ignored
// files were never in it and stay.
TEST(checkpoint_restore_brings_the_tree_back) {
    agent::test::XdgGuard xdg("checkpoint_restore");
    TempProject project("restore", /*git=*/true);
    project.write("a.txt", "one\n");
    project.write("keep.txt", "keep\n");
    project.write(".gitignore", "ignored.txt\n");
    project.commit_all("init");

    agent::CheckpointStore store(project.str(), "sess-restore");
    std::string error;
    const auto base = store.create(0, "base", error, agent::CheckpointPhase::Post);
    ASSERT(base.has_value());

    project.write("a.txt", "two\n");
    project.write("added.txt", "new\n");
    project.write("ignored.txt", "ignore\n");
    ASSERT(store.create(1, "change", error).has_value());

    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT_EQ(project.read("keep.txt"), std::string("keep\n"));
    ASSERT_FALSE(project.exists("added.txt"));
    ASSERT_TRUE(project.exists("ignored.txt"));
}

// An interrupted checkpoint can leave the private index's lock behind. The
// index belongs to this session, so a lock present when the next operation
// starts is stale and must be cleared; a restore otherwise fails with
// "index.lock exists" and the TUI stays on the confirmation frame.
// A project can keep its harness state and cache inside its own tree and list
// them in .gitignore. The checkpoint's `git add -A` excludes those roots by
// pathspec, but git refuses the whole add when an exclude names a path its own
// .gitignore already covers ("The following paths are ignored"). An ignored
// path is skipped by `-A` anyway, so the snapshot must not name it.
TEST(checkpoint_stages_a_project_that_ignores_its_xdg_roots) {
    TempProject project("ignored_xdg_roots", /*git=*/true);
    project.write(".gitignore", "state/\ncache/\n");
    project.write("a.txt", "one\n");
    project.commit_all("init");
    fs::create_directories(project.path() / "state");
    fs::create_directories(project.path() / "cache");

    const std::string root = fs::canonical(project.path()).string();
    struct EnvOverride {
        const char* name;
        bool had = false;
        std::string saved;
        EnvOverride(const char* key, const std::string& value) : name(key) {
            const char* old = std::getenv(key);
            had = old != nullptr;
            if (old)
                saved = old;
            setenv(key, value.c_str(), 1);
        }
        ~EnvOverride() {
            if (had)
                setenv(name, saved.c_str(), 1);
            else
                unsetenv(name);
        }
    } state_env("XDG_STATE_HOME", root + "/state"), cache_env("XDG_CACHE_HOME", root + "/cache");

    agent::CheckpointStore store(root, "sess-ignored-xdg");
    std::string error;
    const auto checkpoint = store.create(1, "change", error);
    ASSERT_TRUE(checkpoint.has_value());
    ASSERT_TRUE(error.empty());
    const agent::GitResult tree =
        agent::run_git(root, {"ls-tree", "-r", "--name-only", checkpoint->id});
    ASSERT_EQ(tree.status, 0);
    ASSERT_EQ(tree.output.find("state/"), std::string::npos);
    ASSERT_EQ(tree.output.find("cache/"), std::string::npos);
    ASSERT_TRUE(tree.output.find("a.txt") != std::string::npos);
}

TEST(checkpoint_restore_clears_a_stale_private_index_lock) {
    agent::test::XdgGuard xdg("checkpoint_stale_lock");
    TempProject project("stale_lock", /*git=*/true);
    project.write("a.txt", "one\n");
    project.commit_all("init");

    const std::string root = fs::canonical(project.path()).string();
    agent::CheckpointStore store(root, "sess-stale-lock");
    std::string error;
    const auto base = store.create(0, "base", error);
    ASSERT(base.has_value());
    project.write("a.txt", "two\n");
    ASSERT(store.create(1, "change", error).has_value());

    const fs::path state = fs::path(agent::Workspace::state_dir_for(root)) / "checkpoints";
    ASSERT_TRUE(fs::exists(state));
    const fs::path index = state / "sess-stale-lock.index";
    {
        std::ofstream lock(index.string() + ".lock");
        ASSERT(lock.good());
    }

    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT_FALSE(fs::exists(index.string() + ".lock"));
}

TEST(checkpoint_restore_never_moves_head_or_the_user_index) {
    agent::test::XdgGuard xdg("checkpoint_restore_index");
    TempProject project("restore_index", /*git=*/true);
    project.write("a.txt", "one\n");
    project.commit_all("init");

    project.write("a.txt", "staged\n");
    const agent::GitResult staged = project.git({"add", "a.txt"});
    ASSERT_EQ(staged.status, 0);
    const std::string head = trimmed(project.git({"rev-parse", "HEAD"}).output);
    const std::string cached = project.git({"diff", "--cached", "--name-only"}).output;

    agent::CheckpointStore store(project.str(), "sess-restore-index");
    std::string error;
    const auto base = store.create(0, "base", error);
    ASSERT(base.has_value());
    project.write("a.txt", "later\n");
    ASSERT(store.create(1, "change", error).has_value());

    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(trimmed(project.git({"rev-parse", "HEAD"}).output), head);
    ASSERT_EQ(project.git({"diff", "--cached", "--name-only"}).output, cached);
}

TEST(checkpoint_restore_works_in_a_shadow_repo) {
    agent::test::XdgGuard xdg("checkpoint_restore_shadow");
    TempProject project("restore_shadow", /*git=*/false);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-restore-shadow");
    ASSERT(store.shadow());
    std::string error;
    const auto base = store.create(0, "base", error);
    ASSERT(base.has_value());
    project.write("a.txt", "two\n");
    project.write("added.txt", "new\n");
    ASSERT(store.create(1, "change", error).has_value());

    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT_FALSE(project.exists("added.txt"));
    ASSERT_FALSE(fs::exists(project.path() / ".git"));
}

TEST(checkpoint_restore_reports_an_unknown_tree) {
    agent::test::XdgGuard xdg("checkpoint_restore_unknown");
    TempProject project("restore_unknown", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-restore-unknown");
    agent::Checkpoint bogus;
    bogus.id = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
    std::string error;
    ASSERT_FALSE(store.restore(bogus, error));
    ASSERT_FALSE(error.empty());
}

// The pre-turn snapshot is a distinct entry: it can carry the same turn number
// as the post-turn snapshot, and `find` prefers the post one (the turn's own
// result) while the message records which phase it was.
TEST(checkpoint_phase_is_part_of_the_message) {
    agent::test::XdgGuard xdg("checkpoint_phase");
    TempProject project("phase", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-phase");
    std::string error;
    ASSERT(store.create(1, "post", error).has_value());
    project.write("a.txt", "two\n");
    const auto pre = store.create(1, "before turn 2", error, agent::CheckpointPhase::Pre);
    ASSERT(pre.has_value());
    ASSERT_TRUE(pre->pre());

    const auto list = store.list(error);
    ASSERT_EQ(list.size(), 2u);
    ASSERT_TRUE(list[0].pre());
    ASSERT_FALSE(list[1].pre());

    const auto found = store.find("1", error);
    ASSERT(found.has_value());
    ASSERT_FALSE(found->pre());

    const agent::GitResult message = project.git({"show", "-s", "--format=%B", pre->id});
    ASSERT(message.output.find("Phase: pre") != std::string::npos);
}

TEST(checkpoint_pre_snapshot_without_a_subject) {
    agent::test::XdgGuard xdg("checkpoint_pre_subject");
    TempProject project("pre_subject", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-pre-subject");
    std::string error;
    const auto pre = store.create(0, "", error, agent::CheckpointPhase::Pre);
    ASSERT(pre.has_value());
    const agent::GitResult message = project.git({"show", "-s", "--format=%B", pre->id});
    ASSERT(message.output.find("amber: before turn 1") != std::string::npos);
}

// The service owns the turn boundary and the revert rule: a before snapshot
// records the state the turn starts from, an after snapshot records the turn,
// and an unchanged tree writes neither.
TEST(checkpoint_service_snapshots_before_and_after_a_turn) {
    agent::test::XdgGuard xdg("checkpoint_service_turns");
    TempProject project("service_turns", /*git=*/true);
    project.write("a.txt", "zero\n");

    agent::CheckpointService service(project.str(), "sess-service");
    const auto before = service.before_turn();
    ASSERT_EQ(before.kind, agent::CheckpointChangeKind::Created);
    ASSERT_EQ(before.checkpoint.turn, 0u);
    ASSERT_TRUE(before.checkpoint.pre());

    project.write("a.txt", "one\n");
    const auto after = service.after_turn("first turn");
    ASSERT_EQ(after.kind, agent::CheckpointChangeKind::Created);
    ASSERT_EQ(after.checkpoint.turn, 1u);
    ASSERT_FALSE(after.checkpoint.pre());

    const auto unchanged = service.before_turn();
    ASSERT_EQ(unchanged.kind, agent::CheckpointChangeKind::None);

    project.write("a.txt", "two\n");
    const auto manual = service.before_turn();
    ASSERT_EQ(manual.kind, agent::CheckpointChangeKind::Created);
    ASSERT_EQ(manual.checkpoint.turn, 1u);
    ASSERT_TRUE(manual.checkpoint.pre());

    project.write("a.txt", "three\n");
    const auto second = service.after_turn("second turn");
    ASSERT_EQ(second.kind, agent::CheckpointChangeKind::Created);
    ASSERT_EQ(second.checkpoint.turn, 2u);
    ASSERT_FALSE(second.checkpoint.pre());
}

// Undo restores the previous checkpoint and records the revert, so the undo is
// itself undoable. The second undo returns to the state before the first.
TEST(checkpoint_service_undo_restores_and_records_the_revert) {
    agent::test::XdgGuard xdg("checkpoint_service_undo");
    TempProject project("service_undo", /*git=*/true);
    project.write("a.txt", "zero\n");

    agent::CheckpointService service(project.str(), "sess-undo");
    ASSERT_EQ(service.before_turn().kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "one\n");
    const auto first = service.after_turn("first");
    ASSERT_EQ(first.kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "two\n");
    ASSERT_EQ(service.after_turn("second").kind, agent::CheckpointChangeKind::Created);

    const auto undo = service.undo();
    ASSERT_EQ(undo.kind, agent::CheckpointChangeKind::Restored);
    ASSERT_EQ(undo.target.turn, first.checkpoint.turn);
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT_EQ(undo.checkpoint.turn, 3u);

    const auto redo = service.undo();
    ASSERT_EQ(redo.kind, agent::CheckpointChangeKind::Restored);
    ASSERT_EQ(project.read("a.txt"), std::string("two\n"));
    ASSERT_EQ(redo.checkpoint.turn, 4u);
}

TEST(checkpoint_service_rollback_reaches_an_older_turn) {
    agent::test::XdgGuard xdg("checkpoint_service_rollback");
    TempProject project("service_rollback", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointService service(project.str(), "sess-rollback");
    ASSERT_EQ(service.before_turn().kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "two\n");
    ASSERT_EQ(service.after_turn("first").kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "three\n");
    ASSERT_EQ(service.after_turn("second").kind, agent::CheckpointChangeKind::Created);

    const auto rolled = service.rollback("0");
    ASSERT_EQ(rolled.kind, agent::CheckpointChangeKind::Restored);
    ASSERT_EQ(rolled.target.turn, 0u);
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT_EQ(rolled.checkpoint.turn, 3u);
}

TEST(checkpoint_service_without_an_earlier_checkpoint_refuses_undo) {
    agent::test::XdgGuard xdg("checkpoint_service_undo_empty");
    TempProject project("service_undo_empty", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointService service(project.str(), "sess-undo-empty");
    ASSERT_EQ(service.after_turn("only").kind, agent::CheckpointChangeKind::Created);
    const auto undo = service.undo();
    ASSERT_EQ(undo.kind, agent::CheckpointChangeKind::Failed);
    ASSERT_FALSE(undo.error.empty());
}

TEST(checkpoint_turn_reports_created_snapshots_only) {
    agent::test::XdgGuard xdg("checkpoint_turn_reports");
    TempProject project("turn_reports", /*git=*/true);
    project.write("a.txt", "zero\n");

    std::vector<std::string> lines;
    agent::CheckpointTurn turn(project.str(), "sess-turn",
                               [&lines](const std::string& line) { lines.push_back(line); });
    turn.before();
    ASSERT_EQ(lines.size(), 1u);
    ASSERT(lines[0].find("checkpoint:") == 0);

    project.write("a.txt", "one\n");
    turn.after("first");
    ASSERT_EQ(lines.size(), 2u);
    ASSERT(lines[1].find("turn 1") != std::string::npos);

    turn.before();
    ASSERT_EQ(lines.size(), 2u);

    project.write("a.txt", "two\n");
    turn.before();
    ASSERT_EQ(lines.size(), 3u);
    ASSERT(lines[2].find("before turn 2") != std::string::npos);
}

// The agent tool resolves the running window's root and session through the
// run scope, so the same tool instance serves every window. A restore is
// approval-gated and records its revert.
TEST(checkpoint_tool_lists_diffs_and_restores) {
    agent::test::XdgGuard xdg("checkpoint_tool");
    TempProject project("tool", /*git=*/true);
    project.write("a.txt", "zero\n");

    agent::CheckpointService service(project.str(), "sess-tool");
    ASSERT_EQ(service.before_turn().kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "one\n");
    ASSERT_EQ(service.after_turn("first").kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "two\n");
    ASSERT_EQ(service.after_turn("second").kind, agent::CheckpointChangeKind::Created);

    const std::string root = project.str();
    const std::string session = "sess-tool";
    agent::RunScope scope;
    scope.workspace_root = &root;
    scope.session_id = &session;
    agent::ScopedRunScope guard(scope);
    const auto tool = agent::make_checkpoint_tool();
    ASSERT_EQ(tool->name(), std::string("git_checkpoint"));
    ASSERT_FALSE(tool->is_read_only());
    ASSERT_FALSE(tool->requires_approval({{"action", "list"}}));
    ASSERT_TRUE(tool->requires_approval({{"action", "restore"}}));
    ASSERT_FALSE(tool->requires_approval(agent::json::object()));
    ASSERT_FALSE(tool->requires_approval(agent::json()));
    ASSERT_FALSE(tool->requires_approval({{"action", 42}}));
    ASSERT_FALSE(tool->requires_approval({{"action", "diff"}}));
    ASSERT_EQ(tool->parameters_schema()["properties"]["action"]["enum"].size(), 3u);
    ASSERT(tool->summarize({{"action", "list"}}).find("checkpoint list") != std::string::npos);
    ASSERT(tool->summarize({{"action", "list"}, {"target", "1"}}).find("checkpoint list 1") !=
           std::string::npos);
    ASSERT(tool->summarize({{"action", "list"}, {"turn", "2"}}).find("checkpoint list 2") !=
           std::string::npos);
    ASSERT(tool->summarize({{"action", "list"}, {"sha", "abc"}}).find("checkpoint list abc") !=
           std::string::npos);
    ASSERT(tool->summarize({{"action", "restore"}, {"target", "1"}}).find("restore") !=
           std::string::npos);
    ASSERT(tool->summarize({{"action", "restore"}}).find("missing target") != std::string::npos);

    const agent::ToolResult latest = tool->execute({{"action", "diff"}});
    ASSERT(latest.ok);
    ASSERT(latest.output.find("latest") != std::string::npos);

    const agent::ToolResult listed = tool->execute({{"action", "list"}});
    ASSERT(listed.ok);
    ASSERT(listed.output.find("turn 2") != std::string::npos);
    ASSERT_EQ(listed.meta["count"].get<std::size_t>(), 3u);
    ASSERT(listed.meta["checkpoints"][0]["pre"].is_boolean());

    const agent::ToolResult diff = tool->execute({{"action", "diff"}, {"target", "2"}});
    ASSERT(diff.ok);
    ASSERT(diff.output.find("-one") != std::string::npos);
    ASSERT(diff.output.find("+two") != std::string::npos);

    const agent::ToolResult restored = tool->execute({{"action", "restore"}, {"target", "1"}});
    ASSERT(restored.ok);
    ASSERT_EQ(project.read("a.txt"), std::string("one\n"));
    ASSERT(restored.meta["revert"].get<std::string>().size() > 0);

    const agent::ToolResult missing = tool->execute({{"action", "restore"}, {"target", "99"}});
    ASSERT_FALSE(missing.ok);
    ASSERT(missing.error.find("no checkpoint") != std::string::npos);
}

TEST(checkpoint_tool_reports_an_empty_history) {
    agent::test::XdgGuard xdg("checkpoint_tool_empty");
    TempProject project("tool_empty", /*git=*/true);
    const std::string root = project.str();
    const std::string session = "sess-tool-empty";
    agent::RunScope scope;
    scope.workspace_root = &root;
    scope.session_id = &session;
    agent::ScopedRunScope guard(scope);

    const auto tool = agent::make_checkpoint_tool();
    const agent::ToolResult listed = tool->execute({{"action", "list"}});
    ASSERT(listed.ok);
    ASSERT_EQ(listed.meta["count"].get<std::size_t>(), 0u);

    const agent::ToolResult missing = tool->execute({{"action", "restore"}});
    ASSERT_FALSE(missing.ok);
    ASSERT(missing.error.find("missing target") != std::string::npos);
}

// A state or cache root placed inside the project is harness bookkeeping, not
// project content. The shadow repo must not stage its own index and object
// store, and restore must not delete those roots.
TEST(checkpoint_excludes_state_and_cache_inside_the_project) {
    agent::test::XdgGuard xdg("checkpoint_own_state");
    TempProject project("own_state", /*git=*/false);
    const std::string project_root = project.str();
    setenv("XDG_STATE_HOME", (project_root + "/state").c_str(), 1);
    setenv("XDG_CACHE_HOME", (project_root + "/cache").c_str(), 1);

    project.write("a.txt", "one\n");
    const std::string state = agent::Workspace::state_dir_for(project_root);
    fs::create_directories(state + "/logs");
    {
        std::ofstream out(state + "/logs/turn.jsonl");
        out << "{}\n";
    }
    fs::create_directories(agent::cache_dir());
    {
        std::ofstream out(agent::cache_dir() + "/model.json");
        out << "{}\n";
    }

    agent::CheckpointStore store(project_root, "sess-own-state");
    ASSERT(store.shadow());
    std::string error;
    const auto base = store.create(0, "base", error);
    ASSERT(base.has_value());
    ASSERT_EQ(base->files, 1u);

    const std::string shadow = state + "/checkpoints/sess-own-state.git";
    const agent::GitResult tree =
        agent::run_git(shadow, {"ls-tree", "-r", "--name-only", base->id});
    ASSERT_EQ(tree.status, 0);
    ASSERT(tree.output.find("a.txt") != std::string::npos);
    ASSERT(tree.output.find("state/") == std::string::npos);
    ASSERT(tree.output.find("cache/") == std::string::npos);

    project.write("a.txt", "two\n");
    ASSERT(store.create(1, "change", error).has_value());
    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(fs::exists(state + "/logs/turn.jsonl"));
    ASSERT_TRUE(fs::exists(agent::cache_dir() + "/model.json"));
}

TEST(checkpoint_format_rows_clip_and_name_the_phase) {
    std::vector<agent::Checkpoint> checkpoints(4);
    checkpoints[0].id = "abcdef0123456789";
    checkpoints[0].turn = 3;
    checkpoints[0].files = 2;
    checkpoints[0].timestamp_s = 0;
    checkpoints[0].prompt = std::string(200, 'x');
    checkpoints[1].id = "1234567890";
    checkpoints[1].turn = 4;
    checkpoints[1].phase = agent::CheckpointPhase::Pre;
    checkpoints[1].prompt = std::string(79, 'a') + "\u00e9" + std::string(50, 'b');
    checkpoints[1].timestamp_s = 1700000000;
    checkpoints[2].id = "short";
    checkpoints[2].turn = 5;
    checkpoints[2].prompt = "  lead\r  spaces";
    checkpoints[2].timestamp_s = std::numeric_limits<long long>::max();
    checkpoints[3].id = "abcdefgh";
    checkpoints[3].turn = 6;
    checkpoints[3].prompt = std::string(81, '\x80');

    const auto rows = agent::checkpoint_rows(checkpoints);
    ASSERT_EQ(rows.size(), 4u);
    ASSERT(rows[0].find("turn 3") != std::string::npos);
    ASSERT(rows[0].find("unknown") != std::string::npos);
    ASSERT(rows[0].find("...") != std::string::npos);
    ASSERT(rows[1].find("before turn 5") != std::string::npos);
    ASSERT(rows[1].find("...") != std::string::npos);
    ASSERT(rows[2].find("turn 5") != std::string::npos);
    ASSERT(rows[2].find("short") != std::string::npos);
    ASSERT(rows[2].find("lead spaces") != std::string::npos);
    ASSERT(rows[2].find("unknown") != std::string::npos);
    ASSERT_EQ(agent::checkpoint_history_text(checkpoints),
              rows[0] + "\n" + rows[1] + "\n" + rows[2] + "\n" + rows[3]);
    ASSERT_EQ(agent::checkpoint_when(-1), std::string("unknown"));
    ASSERT_EQ(agent::checkpoint_describe(checkpoints[0]), std::string("turn 3 (abcdef01)"));
}

TEST(checkpoint_restore_removes_nested_files) {
    agent::test::XdgGuard xdg("checkpoint_restore_dirs");
    TempProject project("restore_dirs", /*git=*/true);
    project.write("keep/a.txt", "one\n");

    agent::CheckpointStore store(project.str(), "sess-restore-dirs");
    std::string error;
    const auto base = store.create(0, "base", error);
    ASSERT(base.has_value());
    project.write("nested/deep/added.txt", "new\n");
    ASSERT(store.create(1, "add", error).has_value());

    ASSERT(store.restore(*base, error));
    ASSERT_TRUE(error.empty());
    ASSERT_FALSE(project.exists("nested/deep/added.txt"));
    ASSERT_EQ(project.read("keep/a.txt"), std::string("one\n"));
}

TEST(checkpoint_service_reports_empty_and_unknown_selections) {
    agent::test::XdgGuard xdg("checkpoint_service_selectors");
    TempProject project("service_selectors", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointService service(project.str(), "sess-selectors");
    std::string error;
    ASSERT(service.diff("", error).empty());
    ASSERT(error.find("no checkpoints") != std::string::npos);
    error.clear();
    ASSERT(service.diff("missing", error).empty());
    ASSERT(error.find("no checkpoint") != std::string::npos);

    const auto rollback = service.rollback("missing");
    ASSERT_EQ(rollback.kind, agent::CheckpointChangeKind::Failed);
    ASSERT(rollback.error.find("no checkpoint") != std::string::npos);
}

TEST(checkpoint_service_reports_a_failed_history_and_create) {
    agent::test::XdgGuard xdg("checkpoint_service_failures");
    TempProject project("service_failures", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointService service(project.str(), "sess-failures");
    {
        FakeGit fake("rev-list");
        const auto before = service.before_turn();
        ASSERT_EQ(before.kind, agent::CheckpointChangeKind::Failed);
        ASSERT_FALSE(before.error.empty());
        const auto after = service.after_turn("turn");
        ASSERT_EQ(after.kind, agent::CheckpointChangeKind::Failed);
    }
    {
        FakeGit fake("rev-list");
        std::string error;
        ASSERT(service.diff("", error).empty());
        ASSERT_FALSE(error.empty());
        error.clear();
        ASSERT(service.diff("1", error).empty());
        ASSERT_FALSE(error.empty());
        ASSERT_EQ(service.undo().kind, agent::CheckpointChangeKind::Failed);
        ASSERT_EQ(service.rollback("1").kind, agent::CheckpointChangeKind::Failed);
    }
    {
        FakeGit fake("add");
        const auto change = service.after_turn("turn");
        ASSERT_EQ(change.kind, agent::CheckpointChangeKind::Failed);
        ASSERT(change.error.find("could not stage") != std::string::npos);
    }
}

TEST(checkpoint_service_reports_a_failed_restore_and_revert_record) {
    agent::test::XdgGuard xdg("checkpoint_service_restore_failures");
    TempProject project("service_restore_failures", /*git=*/true);
    project.write("a.txt", "one\n");

    agent::CheckpointService service(project.str(), "sess-restore-failures");
    ASSERT_EQ(service.after_turn("first").kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "two\n");
    ASSERT_EQ(service.after_turn("second").kind, agent::CheckpointChangeKind::Created);
    {
        FakeGit fake("read-tree");
        const auto change = service.rollback("1");
        ASSERT_EQ(change.kind, agent::CheckpointChangeKind::Failed);
        ASSERT(change.error.find("could not read the checkpoint tree") != std::string::npos);
    }
    {
        FakeGit fake("commit-tree");
        const auto change = service.undo();
        ASSERT_EQ(change.kind, agent::CheckpointChangeKind::Restored);
        ASSERT_FALSE(change.error.empty());
        ASSERT(change.error.find("could not record the revert") != std::string::npos);
    }
}

TEST(checkpoint_turn_reports_a_failed_snapshot) {
    agent::test::XdgGuard xdg("checkpoint_turn_failed");
    TempProject project("turn_failed", /*git=*/true);
    project.write("a.txt", "one\n");

    FakeGit fake("add");
    std::vector<std::string> lines;
    agent::CheckpointTurn turn(project.str(), "sess-turn-failed",
                               [&lines](const std::string& line) { lines.push_back(line); });
    turn.before();
    ASSERT_EQ(lines.size(), 1u);
    ASSERT(lines[0].find("checkpoint:") == 0);
    ASSERT(lines[0].find("could not stage") != std::string::npos);
}

TEST(checkpoint_tool_reports_git_failures_and_truncates) {
    agent::test::XdgGuard xdg("checkpoint_tool_failures");
    TempProject project("tool_failures", /*git=*/true);
    project.write("a.txt", "one\n");
    project.write("big.txt", std::string(70000, 'x'));

    agent::CheckpointService service(project.str(), "sess-tool-failures");
    ASSERT_EQ(service.after_turn("big").kind, agent::CheckpointChangeKind::Created);
    project.write("a.txt", "two\n");
    ASSERT_EQ(service.after_turn("second").kind, agent::CheckpointChangeKind::Created);

    const std::string root = project.str();
    const std::string session = "sess-tool-failures";
    agent::RunScope scope;
    scope.workspace_root = &root;
    scope.session_id = &session;
    agent::ScopedRunScope guard(scope);
    const auto tool = agent::make_checkpoint_tool();

    const agent::ToolResult truncated = tool->execute({{"action", "diff"}, {"target", "1"}});
    ASSERT(truncated.ok);
    ASSERT_EQ(truncated.meta["truncated"].get<bool>(), true);
    ASSERT(truncated.output.find("[diff truncated]") != std::string::npos);

    {
        FakeGit fake("rev-list");
        const agent::ToolResult listed = tool->execute({{"action", "list"}});
        ASSERT_FALSE(listed.ok);
        ASSERT(listed.error.find("git_checkpoint list") != std::string::npos);
    }
    {
        FakeGit fake("diff");
        const agent::ToolResult diff = tool->execute({{"action", "diff"}, {"target", "1"}});
        ASSERT_FALSE(diff.ok);
        ASSERT(diff.error.find("git_checkpoint diff") != std::string::npos);
    }
    {
        FakeGit fake("commit-tree");
        const agent::ToolResult restored = tool->execute({{"action", "restore"}, {"target", "1"}});
        ASSERT_FALSE(restored.ok);
        ASSERT(restored.output.find("restored") != std::string::npos);
        ASSERT(restored.error.find("could not record the revert") != std::string::npos);
    }

    const agent::ToolResult unknown = tool->execute({{"action", "explode"}});
    ASSERT_FALSE(unknown.ok);
    const agent::ToolResult wrong_type = tool->execute({{"action", 42}});
    ASSERT_FALSE(wrong_type.ok);
    ASSERT(wrong_type.error.find("git_checkpoint:") == 0);
}

TEST(checkpoint_plugin_names_itself) {
    agent::plugins::ToolCheckpointPlugin plugin;
    ASSERT_EQ(plugin.id(), std::string("tool_checkpoint"));
    ASSERT_EQ(plugin.name(), std::string("Checkpoint tool"));
    ASSERT_FALSE(plugin.capabilities().empty());
}
