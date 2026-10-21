
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "agent/session.h"
#include "agent/worktree.h"
#include "tests/test_util.h"

namespace fs = std::filesystem;

namespace {

// A throwaway repository with one commit: worktree operations need a base
// commit, and a real repository is the only honest fixture for git behaviour.
struct TempRepo {
    fs::path dir;

    TempRepo() {
        static int counter = 0;
        dir = fs::temp_directory_path() /
              ("amber_wt_repo_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string cmd = "cd '" + dir.string() +
                                "' && git init -q -b main . && echo base > f.txt && git add f.txt "
                                "&& git -c user.email=t@t -c user.name=t commit -qm init";
        if (std::system(cmd.c_str()) != 0)
            agent::test::fail("temp repo setup failed: " + cmd);
    }

    ~TempRepo() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string head_oid() const { return agent::git_head_oid(dir.string()); }
    std::string worktree_path(const std::string& name) const {
        return agent::worktree_base_dir(dir.string()) + "/" + name;
    }
};

void run_in_dir(const fs::path& dir, const std::string& cmd) {
    if (std::system(("cd '" + dir.string() + "' && " + cmd).c_str()) != 0)
        agent::test::fail("command failed: " + cmd);
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A pid that is provably not running: a child that has been reaped. A made-up
// number could exist on a machine with a raised pid_max.
long dead_pid() {
    const pid_t child = fork();
    if (child == 0)
        _exit(0);
    int status = 0;
    waitpid(child, &status, 0);
    return static_cast<long>(child);
}

fs::path temp_state_dir(const std::string& tag) {
    const fs::path dir =
        fs::temp_directory_path() / ("amber_wt_state_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

agent::WorktreeEntry entry_for(const std::string& name, const std::string& path) {
    agent::WorktreeEntry e;
    e.name = name;
    e.path = path;
    e.base_oid = "base";
    e.head_oid = "head";
    e.pid = dead_pid();
    e.start_ticks = 1;
    return e;
}

} // namespace

TEST(worktree_name_validation_rejects_paths_and_dots) {
    ASSERT_TRUE(agent::valid_worktree_name("fix-auth"));
    ASSERT_TRUE(agent::valid_worktree_name("w1.2"));
    ASSERT_FALSE(agent::valid_worktree_name(""));
    ASSERT_FALSE(agent::valid_worktree_name(".."));
    ASSERT_FALSE(agent::valid_worktree_name("a/b"));
    ASSERT_FALSE(agent::valid_worktree_name(".hidden"));
    ASSERT_FALSE(agent::valid_worktree_name("-leading"));
    ASSERT_FALSE(agent::valid_worktree_name(std::string(65, 'a')));
}

TEST(worktree_base_dir_is_under_the_temp_root) {
    const std::string base = agent::worktree_base_dir("/home/u/projects/amber");
    ASSERT(base.find("/amber-wt/") != std::string::npos);
    ASSERT(base.find("home-u-projects-amber") != std::string::npos);
    ASSERT_FALSE(base.empty());
    ASSERT(base.back() != '/');
}

TEST(worktree_git_detection_says_what_it_is) {
    TempRepo repo;
    ASSERT_TRUE(agent::is_git_repo(repo.dir.string()));
    ASSERT_FALSE(repo.head_oid().empty());

    const fs::path plain = fs::temp_directory_path() / "amber_wt_plain";
    fs::remove_all(plain);
    fs::create_directories(plain);
    ASSERT_FALSE(agent::is_git_repo(plain.string()));
    ASSERT_FALSE(agent::is_git_repo("/definitely/not/a/directory"));
    ASSERT_TRUE(agent::git_head_oid(plain.string()).empty());
    fs::remove_all(plain);
}

TEST(worktree_create_checks_out_the_base_commit_detached) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const std::string name = "feature-one";
    const std::string path = repo.worktree_path(name);

    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));
    ASSERT_TRUE(err.empty());

    ASSERT_TRUE(fs::exists(fs::path(path) / "f.txt"));
    ASSERT_EQ(agent::git_head_oid(path), base);

    // Detached: the worktree's own HEAD holds a raw oid, not a branch ref.
    const std::string head = read_file(repo.dir / ".git" / "worktrees" / name / "HEAD");
    ASSERT_EQ(head.find("ref:"), std::string::npos);
    ASSERT_EQ(head.find(base.substr(0, 12)), 0u);

    // Creating over an occupied path fails loudly rather than adopting it.
    const std::string occupied = repo.worktree_path("occupied");
    fs::create_directories(occupied);
    run_in_dir(occupied, "echo something > keep.txt");
    ASSERT_FALSE(agent::create_worktree(repo.dir.string(), occupied, base, err));
    ASSERT_FALSE(err.empty());

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

TEST(worktree_session_note_warns_only_for_a_detached_worktree) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const std::string path = repo.worktree_path("note");

    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));
    ASSERT_TRUE(err.empty());

    const std::string note = agent::worktree_session_note(path);
    ASSERT(note.find("detached git worktree") != std::string::npos);
    ASSERT(note.find(base.substr(0, 7)) != std::string::npos);
    ASSERT(note.find("git switch -c") != std::string::npos);

    // The main checkout is on a branch: nothing to warn about.
    ASSERT_EQ(agent::worktree_session_note(repo.dir.string()), std::string());
    // Not a repository at all.
    ASSERT_EQ(agent::worktree_session_note("/tmp"), std::string());

    // Once the worktree is on a branch the commits are reachable, and the
    // warning goes away.
    run_in_dir(path, "git switch -q -c fix/note");
    ASSERT_EQ(agent::worktree_session_note(path), std::string());

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

TEST(worktree_git_files_matching_respects_ignore_and_nested_repos) {
    TempRepo repo;
    run_in_dir(repo.dir, "mkdir -p pkg ignored");
    run_in_dir(repo.dir, "printf '# Pkg\\n' > pkg/AGENTS.md");
    run_in_dir(repo.dir, "printf 'ignored/\\n' > .gitignore");
    run_in_dir(repo.dir, "printf '# Ignored\\n' > ignored/AGENTS.md");
    run_in_dir(repo.dir, "git add .gitignore pkg/AGENTS.md && "
                         "git -c user.email=t@t -c user.name=t commit -qm rules");
    // A linked worktree inside the checkout, the way a sibling copy sits in a
    // developer's tree.
    run_in_dir(repo.dir, "git worktree add -q --detach sibling HEAD");
    run_in_dir(repo.dir / "sibling", "printf '# Sibling\\n' > AGENTS.md");

    const auto found = agent::git_files_matching(repo.dir.string(), ":(glob)**/AGENTS.md");
    bool pkg = false, ignored = false, sibling = false;
    for (const auto& rel : found) {
        if (rel == "pkg/AGENTS.md")
            pkg = true;
        if (rel.rfind("ignored/", 0) == 0)
            ignored = true;
        if (rel.rfind("sibling/", 0) == 0)
            sibling = true;
    }
    ASSERT(pkg);
    ASSERT_FALSE(ignored);
    ASSERT_FALSE(sibling);
}

TEST(worktree_remove_refuses_a_dirty_tree) {
    TempRepo repo;
    const std::string path = repo.worktree_path("dirty");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, repo.head_oid(), err));

    {
        std::ofstream out(fs::path(path) / "scratch.txt");
        out << "wip\n";
    }
    ASSERT_FALSE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    ASSERT_FALSE(err.empty());
    ASSERT_TRUE(fs::exists(fs::path(path) / "scratch.txt"));

    fs::remove(fs::path(path) / "scratch.txt");
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

TEST(worktree_remove_refuses_commits_off_any_branch) {
    TempRepo repo;
    const std::string path = repo.worktree_path("commits");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, repo.head_oid(), err));

    run_in_dir(path,
               "echo more >> f.txt && git -c user.email=t@t -c user.name=t commit -qam second");

    // The commit exists only on the detached HEAD: removing the worktree now
    // would leave it reachable from no ref, expiring at the next gc.
    ASSERT_FALSE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    ASSERT_FALSE(err.empty());

    run_in_dir(path, "git branch keep-me");
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    ASSERT_FALSE(fs::exists(path));
    ASSERT_FALSE(fs::exists(repo.dir / ".git" / "worktrees" / "commits"));
    // The branch survives the worktree: amber keeps what the user committed.
    ASSERT_TRUE(fs::exists(repo.dir / ".git" / "refs" / "heads" / "keep-me"));
}

TEST(worktree_remove_clean_worktree_succeeds) {
    TempRepo repo;
    const std::string path = repo.worktree_path("clean");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, repo.head_oid(), err));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    ASSERT_FALSE(fs::exists(path));
    ASSERT_FALSE(fs::exists(repo.dir / ".git" / "worktrees" / "clean"));
}

TEST(worktree_registry_round_trips_and_tolerates_damage) {
    const fs::path state = temp_state_dir("roundtrip");
    ASSERT_TRUE(agent::load_worktrees(state.string()).empty());

    agent::WorktreeEntry e;
    e.name = "one";
    e.path = "/tmp/amber-wt/x/one";
    e.base_oid = "abc123";
    e.head_oid = "def456";
    e.session_id = "s-1";
    e.pid = 4242;
    e.start_ticks = 99;
    e.created_ms = 1234567890;
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    const auto loaded = agent::load_worktrees(state.string());
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_EQ(loaded[0].name, "one");
    ASSERT_EQ(loaded[0].path, "/tmp/amber-wt/x/one");
    ASSERT_EQ(loaded[0].session_id, "s-1");
    ASSERT_EQ(loaded[0].pid, 4242L);
    ASSERT_EQ(loaded[0].start_ticks, 99LL);
    ASSERT_EQ(loaded[0].created_ms, 1234567890LL);

    // A damaged registry reads as empty rather than taking the caller down.
    {
        std::ofstream out(state / "worktrees.json");
        out << "{not json";
    }
    ASSERT_TRUE(agent::load_worktrees(state.string()).empty());
    fs::remove_all(state);
}

TEST(worktree_owner_liveness_uses_start_ticks) {
#ifdef __linux__
    agent::WorktreeEntry e;
    e.pid = ::getpid();
    e.start_ticks = agent::process_start_ticks(e.pid);
    ASSERT(e.start_ticks > 0);
    ASSERT(agent::owner_state(e) == agent::OwnerState::Alive);

    // Same pid, wrong start time: the pid was reused, so the owner is gone.
    e.start_ticks += 1;
    ASSERT(agent::owner_state(e) == agent::OwnerState::Dead);

    e.pid = dead_pid();
    e.start_ticks = 1;
    ASSERT(agent::owner_state(e) == agent::OwnerState::Dead);

    // No usable stamp: cannot tell, and "cannot tell" never reaps.
    e.pid = 0;
    ASSERT(agent::owner_state(e) == agent::OwnerState::Unknown);
#else
    // Without procfs a live owner cannot be told from a reused pid; the
    // conservative answer keeps the worktree.
    agent::WorktreeEntry e;
    e.pid = ::getpid();
    e.start_ticks = 1;
    ASSERT(agent::owner_state(e) == agent::OwnerState::Unknown);
#endif
}

// "Cannot tell" must never authorize a removal. A pid we cannot check (a
// registry written elsewhere, a platform without procfs) leaves the worktree
// alone: the alternative is eating a live window's work. macOS CI caught this
// as a behaviour difference, which is what this test pins on every platform.
TEST(worktree_reap_never_touches_an_unknown_owner) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("unknown");
    std::string err;
    const std::string path = repo.worktree_path("unknown");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));

    agent::WorktreeEntry e = entry_for("unknown", path);
    e.base_oid = base;
    e.pid = 0; // no usable owner stamp -> Unknown
    e.start_ticks = 0;
    ASSERT(agent::owner_state(e) == agent::OwnerState::Unknown);
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_TRUE(fs::exists(path));
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    fs::remove_all(state);
}

TEST(worktree_reap_removes_only_dead_owner_worktrees) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("reap");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), repo.worktree_path("dead"), base, err));
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), repo.worktree_path("alive"), base, err));

    agent::WorktreeEntry dead = entry_for("dead", repo.worktree_path("dead"));
    dead.base_oid = base;
    agent::WorktreeEntry alive = entry_for("alive", repo.worktree_path("alive"));
    alive.base_oid = base;
    alive.pid = ::getpid();
    alive.start_ticks = agent::process_start_ticks(alive.pid);
    ASSERT_TRUE(agent::save_worktrees(state.string(), {dead, alive}));

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {});
#ifdef __linux__
    ASSERT_EQ(report.removed.size(), 1u);
    ASSERT_EQ(report.removed[0], "dead");
    ASSERT_FALSE(fs::exists(repo.worktree_path("dead")));

    const auto left = agent::load_worktrees(state.string());
    ASSERT_EQ(left.size(), 1u);
    ASSERT_EQ(left[0].name, "alive");
#else
    // No procfs: every owner reads Unknown, and Unknown never reaps.
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 2u);
#endif
    ASSERT_TRUE(fs::exists(repo.worktree_path("alive")));

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), repo.worktree_path("alive"), err));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), repo.worktree_path("dead"), err));
    fs::remove_all(state);
}

TEST(worktree_reap_keeps_a_dirty_orphan_and_says_why) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("dirty");
    std::string err;
    const std::string path = repo.worktree_path("dirty");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));
    {
        std::ofstream out(fs::path(path) / "uncommitted.txt");
        out << "work in progress\n";
    }

    agent::WorktreeEntry e = entry_for("dirty", path);
    e.base_oid = base;
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_TRUE(fs::exists(path));
#ifdef __linux__
    ASSERT_EQ(report.kept.size(), 1u);
    ASSERT_EQ(report.kept[0].first, "dirty");
    ASSERT_FALSE(report.kept[0].second.empty());
#endif
    // The entry survives with the worktree, so the next run reports it again.
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);

    fs::remove(fs::path(path) / "uncommitted.txt");
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    fs::remove_all(state);
}

TEST(worktree_reap_drops_entries_whose_path_vanished) {
    TempRepo repo;
    const fs::path state = temp_state_dir("vanished");
    ASSERT_TRUE(
        agent::save_worktrees(state.string(), {entry_for("gone", repo.worktree_path("gone"))}));

    // A vanished directory is metadata to tidy on every platform: there is
    // nothing left that a removal could lose.
    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {});
    ASSERT_EQ(report.removed.size(), 1u);
    ASSERT_EQ(report.removed[0], "gone");
    ASSERT_TRUE(agent::load_worktrees(state.string()).empty());
    fs::remove_all(state);
}

// The owner stamp is what a later run uses to tell a live window from a reused
// pid, so creation must fill it rather than leave it to chance.
TEST(worktree_entry_carries_an_owner_stamp) {
    const agent::WorktreeEntry e =
        agent::make_worktree_entry("w1", "/tmp/amber-wt/x/w1", "abc123", "s-7");
    ASSERT_EQ(e.name, "w1");
    ASSERT_EQ(e.path, "/tmp/amber-wt/x/w1");
    ASSERT_EQ(e.base_oid, "abc123");
    ASSERT_EQ(e.head_oid, "abc123"); // a fresh worktree sits at its base
    ASSERT_EQ(e.session_id, "s-7");
    ASSERT_EQ(e.pid, static_cast<long>(::getpid()));
#ifdef __linux__
    ASSERT(e.start_ticks > 0);
#endif
    ASSERT(e.created_ms > 0);
}

// The registry is the only thing that makes a worktree reapable, so adding the
// same name twice must update rather than duplicate, and removing must name it.
TEST(worktree_registry_add_replaces_and_remove_drops) {
    const fs::path state = temp_state_dir("registry");

    agent::WorktreeEntry first = agent::make_worktree_entry("w1", "/tmp/a/w1", "oid1", "s-1");
    ASSERT_TRUE(agent::registry_add(state.string(), first));
    first.base_oid = "oid2";
    ASSERT_TRUE(agent::registry_add(state.string(), first));
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);
    ASSERT_EQ(agent::load_worktrees(state.string())[0].base_oid, "oid2");

    agent::WorktreeEntry other = agent::make_worktree_entry("w2", "/tmp/a/w2", "oid1", "s-2");
    ASSERT_TRUE(agent::registry_add(state.string(), other));
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 2u);

    ASSERT_TRUE(agent::registry_remove(state.string(), "w1"));
    const auto left = agent::load_worktrees(state.string());
    ASSERT_EQ(left.size(), 1u);
    ASSERT_EQ(left[0].name, "w2");
    ASSERT_FALSE(agent::registry_remove(state.string(), "w1"));
    fs::remove_all(state);
}

// A window's workspace must survive a restart: the worktree path, its name and
// the commits it sits at, so a wiped /tmp can be rebuilt exactly.
TEST(window_entry_persists_its_workspace) {
    agent::WorkspaceState ws;
    agent::WorkspaceState::WindowEntry we;
    we.session_id = "s-1";
    we.title = "chat";
    we.prompt_history = {"hello"};
    we.root = "/tmp/amber-wt/proj/w1";
    we.worktree = "w1";
    we.base_oid = "base123";
    we.head_oid = "head456";
    ws.windows.push_back(we);
    ws.active = 0;

    const agent::WorkspaceState back = agent::WorkspaceState::from_json(ws.to_json());
    ASSERT_EQ(back.windows.size(), 1u);
    ASSERT_EQ(back.windows[0].root, "/tmp/amber-wt/proj/w1");
    ASSERT_EQ(back.windows[0].worktree, "w1");
    ASSERT_EQ(back.windows[0].base_oid, "base123");
    ASSERT_EQ(back.windows[0].head_oid, "head456");
    ASSERT_EQ(back.windows[0].session_id, "s-1");
    ASSERT_EQ(back.windows[0].prompt_history.size(), 1u);

    // A workspace file written before per-window workspaces existed reads as
    // "no workspace", which means the project root.
    const agent::json legacy = {
        {"version", 1}, {"active", 0}, {"windows", {{{"session_id", "s-2"}, {"title", "old"}}}}};
    const agent::WorkspaceState old = agent::WorkspaceState::from_json(legacy);
    ASSERT_EQ(old.windows.size(), 1u);
    ASSERT_TRUE(old.windows[0].root.empty());
    ASSERT_TRUE(old.windows[0].worktree.empty());
}

// The path a restored window is about to re-attach to claims its workspace,
// even when the registry entry predates the session id — reaping it would
// destroy the very tree the window is coming back to.
TEST(worktree_reap_leaves_a_claimed_path_alone) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("claimed");
    std::string err;
    const std::string path = repo.worktree_path("claimed");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));

    agent::WorktreeEntry e = entry_for("claimed", path);
    e.base_oid = base;
    e.session_id.clear(); // the window's session id is minted after creation
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {path});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_TRUE(fs::exists(path));
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    fs::remove_all(state);
}

// The background pass, as one call: reap what nothing claims, rebuild what is
// gone, and record where a live workspace stopped.
TEST(workspace_pass_reaps_rebuilds_and_records) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("pass");
    std::string err;

    // An orphan whose owner is gone, plus a window's workspace whose directory
    // has been wiped — /tmp is volatile by design, the commits are not.
    const std::string orphan = repo.worktree_path("orphan");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), orphan, base, err));
    agent::WorktreeEntry entry = entry_for("orphan", orphan);
    entry.base_oid = base;
    ASSERT_TRUE(agent::save_worktrees(state.string(), {entry}));

    const std::string rebuilt = repo.worktree_path("kept");
    agent::WorkspacePass pass;
    pass.project = repo.dir.string();
    pass.state_dir = state.string();
    pass.live_sessions.push_back("s-live");
    pass.live_paths.push_back(rebuilt);
    pass.rebuilds.push_back({7, "kept", base, rebuilt});

    const agent::WorkspacePassResult result = agent::run_workspace_pass(pass);
    ASSERT_EQ(result.rebuilt.size(), 1u);
    ASSERT_EQ(result.rebuilt[0].first, 7u);
    ASSERT_EQ(result.rebuilt[0].second, rebuilt);
    ASSERT_TRUE(fs::exists(rebuilt + "/f.txt")); // rebuilt at the recorded commit
#ifdef __linux__
    // The orphan is gone and said so; without procfs nothing is provably dead.
    ASSERT_EQ(result.notices.size(), 2u);
    ASSERT_FALSE(fs::exists(orphan));
#else
    ASSERT_EQ(result.notices.size(), 1u);
#endif
    bool kept_recorded = false;
    for (const auto& e : agent::load_worktrees(state.string()))
        kept_recorded = kept_recorded || e.name == "kept";
    ASSERT(kept_recorded);

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), rebuilt, err));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), orphan, err));
    fs::remove_all(state);
}

// A rebuild that cannot happen is reported, not silently skipped: the window
// stays on the project root and says why.
TEST(workspace_pass_reports_a_rebuild_it_cannot_do) {
    TempRepo repo;
    const fs::path state = temp_state_dir("pass-fail");
    agent::WorkspacePass pass;
    pass.project = repo.dir.string();
    pass.state_dir = state.string();
    pass.rebuilds.push_back({3, "gone", "not-a-commit", repo.worktree_path("gone")});

    const agent::WorkspacePassResult result = agent::run_workspace_pass(pass);
    ASSERT_EQ(result.rebuilt.size(), 0u);
    ASSERT_EQ(result.notices.size(), 1u);
    ASSERT(result.notices[0].find("could not be rebuilt") != std::string::npos);
    ASSERT_FALSE(fs::exists(repo.worktree_path("gone")));
    fs::remove_all(state);
}

// Giving a workspace back: removed when nothing can be lost, kept with the
// reason when something can.
TEST(workspace_release_removes_only_when_nothing_can_be_lost) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("release");
    std::string err;

    const std::string clean = repo.worktree_path("given-back");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), clean, base, err));
    ASSERT_TRUE(agent::registry_add(state.string(),
                                    agent::make_worktree_entry("given-back", clean, base, "s")));
    const std::string removed =
        agent::release_workspace(repo.dir.string(), state.string(), "given-back", clean);
    ASSERT(removed.find("removed") != std::string::npos);
    ASSERT_FALSE(fs::exists(clean));
    ASSERT_TRUE(agent::load_worktrees(state.string()).empty());

    const std::string dirty = repo.worktree_path("kept");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), dirty, base, err));
    {
        std::ofstream out(fs::path(dirty) / "wip.txt");
        out << "work in progress\n";
    }
    const std::string kept =
        agent::release_workspace(repo.dir.string(), state.string(), "kept", dirty);
    ASSERT(kept.find("kept") != std::string::npos);
    ASSERT_TRUE(fs::exists(fs::path(dirty) / "wip.txt"));

    fs::remove(fs::path(dirty) / "wip.txt");
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), dirty, err));
    fs::remove_all(state);
}

TEST(worktree_reap_leaves_an_attached_session_alone) {
    TempRepo repo;
    const fs::path state = temp_state_dir("attached");
    agent::WorktreeEntry e = entry_for("attached", repo.worktree_path("attached"));
    e.session_id = "s-live";
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    // A restored window re-attaches before the reaper runs; its workspace is
    // spoken for even when the pid that created it is gone.
    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {"s-live"}, {});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);
    fs::remove_all(state);
}

// Parking is an explicit "keep this": an unclaimed parked workspace survives a
// reap pass even when its owner is provably gone, which is what makes
// /worktree detach and /set worktree off non-destructive.
TEST(worktree_parked_entry_survives_reap) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const fs::path state = temp_state_dir("parked");
    std::string err;
    const std::string path = repo.worktree_path("parked-one");
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));

    agent::WorktreeEntry e = entry_for("parked-one", path);
    e.base_oid = base;
    e.session_id.clear();
    e.parked = true;
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {}, {});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_TRUE(fs::exists(path));
    const auto left = agent::load_worktrees(state.string());
    ASSERT_EQ(left.size(), 1u);
    ASSERT_TRUE(left[0].parked);

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
    fs::remove_all(state);
}

// A live window's claim cannot be stolen. A stale session id (nobody claims it
// now) is reclaimable: the registry is a lease, not a permanent identity.
TEST(worktree_claim_refuses_a_live_owner_and_reclaims_a_stale_one) {
    const fs::path state = temp_state_dir("claim");
    agent::WorktreeEntry live = agent::make_worktree_entry("live", "/tmp/a/live", "oid", "s-live");
    ASSERT_TRUE(agent::save_worktrees(state.string(), {live}));

    std::string err;
    ASSERT_FALSE(agent::claim_worktree(state.string(), "live", "s-other", {"s-live"}, err));
    ASSERT(err.find("another window") != std::string::npos);
    ASSERT_EQ(agent::load_worktrees(state.string())[0].session_id, std::string("s-live"));

    // The same window may re-claim its own workspace (a restart path).
    ASSERT_TRUE(agent::claim_worktree(state.string(), "live", "s-live", {"s-live"}, err));

    // A session nobody claims is stale, not live.
    ASSERT_TRUE(agent::claim_worktree(state.string(), "live", "s-new", {"s-new"}, err));
    ASSERT_EQ(agent::load_worktrees(state.string())[0].session_id, std::string("s-new"));

    ASSERT_FALSE(agent::claim_worktree(state.string(), "missing", "s-new", {}, err));
    ASSERT(err.find("no workspace") != std::string::npos);
    fs::remove_all(state);
}

TEST(worktree_park_releases_the_claim_without_removing) {
    const fs::path state = temp_state_dir("park");
    agent::WorktreeEntry e = agent::make_worktree_entry("one", "/tmp/a/one", "oid", "s-1");
    ASSERT_TRUE(agent::save_worktrees(state.string(), {e}));
    ASSERT_TRUE(agent::park_worktree(state.string(), "one"));

    const auto parked = agent::load_worktrees(state.string());
    ASSERT_EQ(parked.size(), 1u);
    ASSERT_TRUE(parked[0].parked);
    ASSERT_TRUE(parked[0].session_id.empty());
    ASSERT_EQ(parked[0].pid, 0L);
    ASSERT_FALSE(agent::park_worktree(state.string(), "missing"));
    fs::remove_all(state);
}

// The status the manager and the leave dialog read: branch, cleanliness and
// whether HEAD is reachable. A dirty tree or an unreferenced HEAD is exactly
// what removal must refuse, so both must be visible before the user chooses.
TEST(worktree_status_reports_dirty_branch_and_reachability) {
    TempRepo repo;
    const std::string base = repo.head_oid();
    const std::string path = repo.worktree_path("status");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, base, err));
    agent::WorktreeEntry e = agent::make_worktree_entry("status", path, base, "s-1");

    agent::WorktreeStatus clean = agent::worktree_status(repo.dir.string(), e, {}, {});
    ASSERT_TRUE(clean.exists);
    ASSERT_FALSE(clean.dirty);
    ASSERT_TRUE(clean.reachable);
    ASSERT_EQ(clean.branch, std::string("detached"));
    ASSERT(clean.state == agent::WorktreeState::Kept);

    {
        std::ofstream out(fs::path(path) / "scratch.txt");
        out << "wip\n";
    }
    agent::WorktreeStatus dirty = agent::worktree_status(repo.dir.string(), e, {"s-1"}, {});
    ASSERT_TRUE(dirty.dirty);
    ASSERT(dirty.problem.find("uncommitted") != std::string::npos);
    ASSERT(dirty.state == agent::WorktreeState::Live);

    fs::remove(fs::path(path) / "scratch.txt");
    run_in_dir(path,
               "echo more >> f.txt && git -c user.email=t@t -c user.name=t commit -qam second");
    agent::WorktreeStatus detached = agent::worktree_status(repo.dir.string(), e, {}, {});
    ASSERT_FALSE(detached.dirty);
    ASSERT_FALSE(detached.reachable); // the new commit is on no ref
    ASSERT(detached.problem.find("no branch") != std::string::npos);

    ASSERT_TRUE(agent::create_branch_at_head(path, "amber-keep", err));
    ASSERT_TRUE(agent::worktree_head_reachable(repo.dir.string(), path));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

// The dirty leave option preserves work where git expects it. A clean tree is
// a success too: the user chose leave, and there was nothing to save.
TEST(worktree_stash_preserves_uncommitted_work) {
    TempRepo repo;
    const std::string path = repo.worktree_path("stash");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, repo.head_oid(), err));
    {
        std::ofstream out(fs::path(path) / "wip.txt");
        out << "work in progress\n";
    }
    ASSERT_FALSE(agent::worktree_dirty_reason(path).empty());
    ASSERT_TRUE(agent::stash_worktree(repo.dir.string(), path, "amber: leave stash", err));
    ASSERT_TRUE(agent::worktree_dirty_reason(path).empty());
    ASSERT(read_file(repo.dir / ".git" / "logs" / "refs" / "stash").find("amber: leave stash") !=
           std::string::npos);
    ASSERT_TRUE(agent::stash_worktree(repo.dir.string(), path, "amber: empty", err));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

// Walking away from a detached HEAD must not leave commits to gc: the explicit
// branch option is what makes the workspace removable.
TEST(worktree_branch_at_head_makes_commits_reachable) {
    TempRepo repo;
    const std::string path = repo.worktree_path("branch");
    std::string err;
    ASSERT_TRUE(agent::create_worktree(repo.dir.string(), path, repo.head_oid(), err));
    run_in_dir(path, "echo more >> f.txt && git -c user.email=t@t -c user.name=t commit -qam work");
    ASSERT_FALSE(agent::worktree_head_reachable(repo.dir.string(), path));
    ASSERT_FALSE(agent::remove_worktree_safe(repo.dir.string(), path, err));

    ASSERT_TRUE(agent::create_branch_at_head(path, "amber-leave-branch", err));
    ASSERT_TRUE(agent::worktree_head_reachable(repo.dir.string(), path));
    ASSERT_TRUE(fs::exists(repo.dir / ".git" / "refs" / "heads" / "amber-leave-branch"));
    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), path, err));
}

TEST(worktree_prune_drops_vanished_entries_and_keeps_existing_parked) {
    TempRepo repo;
    const fs::path state = temp_state_dir("prune");
    agent::WorktreeEntry gone = entry_for("gone", repo.worktree_path("gone"));
    agent::WorktreeEntry parked =
        agent::make_worktree_entry("parked", repo.worktree_path("parked"), "oid", "");
    parked.parked = true;
    fs::create_directories(repo.worktree_path("parked"));
    ASSERT_TRUE(agent::save_worktrees(state.string(), {gone, parked}));

    const auto removed = agent::prune_registry(repo.dir.string(), state.string());
    ASSERT_EQ(removed.size(), 1u);
    ASSERT_EQ(removed[0], "gone");
    const auto left = agent::load_worktrees(state.string());
    ASSERT_EQ(left.size(), 1u);
    ASSERT_EQ(left[0].name, "parked");
    ASSERT_TRUE(left[0].parked);
    fs::remove_all(state);
}
