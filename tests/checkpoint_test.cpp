// Per-session checkpoints: the commit on refs/amber/checkpoints/<session>, the
// structured message that acts as the index, the shadow repo for a project that
// is not a git checkout, and the promise that the user's HEAD and index never
// move.
//
// The store is exercised against real temporary repositories; no git daemon and
// no network.

#include "agent/checkpoint.h"
#include "agent/git_exec.h"
#include "tests/test_util.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

    const auto by_sha = store.find(list[1].id.substr(0, 8), error);
    ASSERT(by_sha.has_value());
    ASSERT_EQ(by_sha->id, list[1].id);

    ASSERT_FALSE(store.find("99", error).has_value());
    ASSERT_TRUE(error.empty());
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
