
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

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

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {});
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

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {});
#ifdef __linux__
    ASSERT_EQ(report.removed.size(), 1u);
    ASSERT_EQ(report.removed[0], "dead");
    ASSERT_FALSE(fs::exists(repo.worktree_path("dead")));
#else
    ASSERT_EQ(report.removed.size(), 0u);
#endif
    ASSERT_TRUE(fs::exists(repo.worktree_path("alive")));

    const auto left = agent::load_worktrees(state.string());
    ASSERT_EQ(left.size(), 1u);
    ASSERT_EQ(left[0].name, "alive");

    ASSERT_TRUE(agent::remove_worktree_safe(repo.dir.string(), repo.worktree_path("alive"), err));
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

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {});
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

    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {});
#ifdef __linux__
    ASSERT_EQ(report.removed.size(), 1u);
    ASSERT_EQ(report.removed[0], "gone");
    ASSERT_TRUE(agent::load_worktrees(state.string()).empty());
#else
    ASSERT_EQ(report.removed.size(), 0u);
#endif
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
    const auto report = agent::reap_worktrees(repo.dir.string(), state.string(), {"s-live"});
    ASSERT_EQ(report.removed.size(), 0u);
    ASSERT_EQ(agent::load_worktrees(state.string()).size(), 1u);
    fs::remove_all(state);
}
