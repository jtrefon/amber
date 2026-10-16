
#include "agent/worktree.h"

#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct GitResult {
    int status = -1;
    std::string output;
};

// Run `git -C <repo> <args...>` with argv exec — never a shell string, because
// worktree names reach this from the model — and capture merged stdout+stderr:
// git's own refusals are the messages the user ends up reading.
GitResult run_git(const std::string& repo, const std::vector<std::string>& args) {
    GitResult result;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return result;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("git"));
        argv.push_back(const_cast<char*>("-C"));
        argv.push_back(const_cast<char*>(repo.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("git", argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[512];
    ssize_t got = 0;
    while ((got = read(fds[0], buf, sizeof buf)) > 0)
        result.output.append(buf, static_cast<std::size_t>(got));
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) > 0 && WIFEXITED(status))
        result.status = WEXITSTATUS(status);
    return result;
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

// Worktrees live under shared /tmp: 0700 keeps a checkout unreadable to other
// users on a multi-user machine. Best effort — a filesystem that refuses the
// change is not a reason to fail the operation.
void tighten(const fs::path& dir) {
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
}

bool reachable_from_any_ref(const std::string& repo, const std::string& oid) {
    const GitResult r = run_git(repo, {"for-each-ref", "--contains", oid});
    return r.status == 0 && !trimmed(r.output).empty();
}

bool claims(const WorktreeEntry& entry, const std::vector<std::string>& sessions,
            const std::vector<std::string>& paths) {
    if (!entry.session_id.empty() &&
        std::find(sessions.begin(), sessions.end(), entry.session_id) != sessions.end())
        return true;
    return std::find(paths.begin(), paths.end(), entry.path) != paths.end();
}

std::string registry_path(const std::string& state_dir) {
    return state_dir + "/worktrees.json";
}

} // namespace

bool valid_worktree_name(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    if (!std::isalnum(static_cast<unsigned char>(name.front())))
        return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_' && c != '.')
            return false;
    }
    return name.find("..") == std::string::npos;
}

std::string worktree_base_dir(const std::string& project_root) {
    const char* tmp = std::getenv("TMPDIR");
    std::string base = (tmp && *tmp) ? std::string(tmp) : std::string("/tmp");
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + "/amber-wt/" + workspace_state_slug(project_root);
}

bool is_git_repo(const std::string& dir) {
    const GitResult r = run_git(dir, {"rev-parse", "--is-inside-work-tree"});
    return r.status == 0 && trimmed(r.output) == "true";
}

std::string git_head_oid(const std::string& dir) {
    const GitResult r = run_git(dir, {"rev-parse", "HEAD"});
    return r.status == 0 ? trimmed(r.output) : std::string();
}

bool create_worktree(const std::string& repo, const std::string& path, const std::string& base_oid,
                     std::string& error) {
    std::error_code ec;
    const fs::path target(path);
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        error = "cannot create " + target.parent_path().string() + ": " + ec.message();
        return false;
    }
    tighten(target.parent_path().parent_path());
    tighten(target.parent_path());
    if (fs::exists(target, ec)) {
        if (!fs::is_empty(target, ec)) {
            error = "path exists and is not empty: " + path;
            return false;
        }
        fs::remove(target, ec);
    }
    const GitResult r = run_git(repo, {"worktree", "add", "--detach", path, base_oid});
    if (r.status != 0) {
        error = trimmed(r.output);
        return false;
    }
    tighten(target);
    return true;
}

bool remove_worktree_safe(const std::string& repo, const std::string& path, std::string& error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        prune_worktrees(repo); // the directory is gone; drop its metadata too
        return true;
    }
    const std::string head = git_head_oid(path);
    if (head.empty()) {
        error = "cannot read HEAD of " + path;
        return false;
    }
    if (!reachable_from_any_ref(repo, head)) {
        error = "commits at " + head.substr(0, 12) +
                " are on no branch; create one before closing the window";
        return false;
    }
    const GitResult r = run_git(repo, {"worktree", "remove", path});
    if (r.status != 0) {
        error = trimmed(r.output);
        return false;
    }
    return true;
}

void prune_worktrees(const std::string& repo) {
    (void)run_git(repo, {"worktree", "prune"});
}

long long process_start_ticks(long pid) {
    if (pid <= 0)
        return -1;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in)
        return -1;
    std::string line;
    std::getline(in, line);
    // `comm` is parenthesized and may itself contain spaces or ')', so the
    // fields start after the LAST ')'. Field 3 (state) is the first of them,
    // which makes starttime — field 22 — the 20th token.
    const std::size_t close = line.rfind(')');
    if (close == std::string::npos)
        return -1;
    std::vector<std::string> fields;
    std::istringstream rest(line.substr(close + 1));
    std::string field;
    while (rest >> field)
        fields.push_back(field);
    if (fields.size() < 20)
        return -1;
    try {
        return std::stoll(fields[19]);
    } catch (...) {
        return -1;
    }
}

OwnerState owner_state(const WorktreeEntry& entry) {
    if (entry.pid <= 0 || entry.start_ticks <= 0)
        return OwnerState::Unknown;
#ifdef __linux__
    const long long start = process_start_ticks(entry.pid);
    if (start <= 0)
        return OwnerState::Dead; // procfs is there and the pid is gone
    return start == entry.start_ticks ? OwnerState::Alive : OwnerState::Dead;
#else
    // Without procfs a live owner cannot be told from a reused pid.
    return OwnerState::Unknown;
#endif
}

std::vector<WorktreeEntry> load_worktrees(const std::string& state_dir) {
    std::vector<WorktreeEntry> entries;
    std::ifstream in(registry_path(state_dir));
    if (!in)
        return entries;
    json doc;
    try {
        in >> doc;
    } catch (const std::exception&) {
        return entries; // a damaged registry reads as empty, never takes the caller down
    }
    if (!doc.is_object() || !doc.contains("worktrees") || !doc["worktrees"].is_array())
        return entries;
    for (const auto& item : doc["worktrees"]) {
        if (!item.is_object())
            continue;
        WorktreeEntry e;
        e.name = item.value("name", "");
        e.path = item.value("path", "");
        e.base_oid = item.value("base_oid", "");
        e.head_oid = item.value("head_oid", "");
        e.session_id = item.value("session_id", "");
        e.pid = item.value("pid", 0L);
        e.start_ticks = item.value("start_ticks", 0LL);
        e.created_ms = item.value("created_ms", 0LL);
        if (!e.name.empty() && !e.path.empty())
            entries.push_back(e);
    }
    return entries;
}

bool save_worktrees(const std::string& state_dir, const std::vector<WorktreeEntry>& entries) {
    std::error_code ec;
    fs::create_directories(state_dir, ec);
    if (ec)
        return false;
    json doc;
    doc["version"] = 1;
    doc["worktrees"] = json::array();
    for (const auto& e : entries) {
        doc["worktrees"].push_back(json{{"name", e.name},
                                        {"path", e.path},
                                        {"base_oid", e.base_oid},
                                        {"head_oid", e.head_oid},
                                        {"session_id", e.session_id},
                                        {"pid", e.pid},
                                        {"start_ticks", e.start_ticks},
                                        {"created_ms", e.created_ms}});
    }
    const std::string path = registry_path(state_dir);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            return false;
        out << doc.dump(2);
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

WorktreeEntry make_worktree_entry(const std::string& name, const std::string& path,
                                  const std::string& base_oid, const std::string& session_id) {
    WorktreeEntry entry;
    entry.name = name;
    entry.path = path;
    entry.base_oid = base_oid;
    entry.head_oid = base_oid; // a fresh worktree sits at its base
    entry.session_id = session_id;
    entry.pid = static_cast<long>(::getpid());
    entry.start_ticks = process_start_ticks(entry.pid);
    entry.created_ms = static_cast<long long>(std::time(nullptr)) * 1000;
    return entry;
}

bool registry_add(const std::string& state_dir, const WorktreeEntry& entry) {
    std::vector<WorktreeEntry> entries = load_worktrees(state_dir);
    const auto same_name = [&](const WorktreeEntry& e) { return e.name == entry.name; };
    const auto existing = std::find_if(entries.begin(), entries.end(), same_name);
    if (existing == entries.end())
        entries.push_back(entry);
    else
        *existing = entry;
    return save_worktrees(state_dir, entries);
}

bool registry_remove(const std::string& state_dir, const std::string& name) {
    std::vector<WorktreeEntry> entries = load_worktrees(state_dir);
    const auto before = entries.size();
    const auto same_name = [&](const WorktreeEntry& e) { return e.name == name; };
    entries.erase(std::remove_if(entries.begin(), entries.end(), same_name), entries.end());
    if (entries.size() == before)
        return false;
    return save_worktrees(state_dir, entries);
}

ReapReport reap_worktrees(const std::string& repo, const std::string& state_dir,
                          const std::vector<std::string>& live_sessions,
                          const std::vector<std::string>& live_paths) {
    ReapReport report;
    std::vector<WorktreeEntry> survivors;
    for (const auto& entry : load_worktrees(state_dir)) {
        // A restored window re-attaches before the reaper runs: its workspace
        // is spoken for however its owner pid looks.
        if (claims(entry, live_sessions, live_paths)) {
            survivors.push_back(entry);
            continue;
        }
        std::error_code ec;
        if (!fs::exists(entry.path, ec)) {
            // Nothing can be lost: the directory is already gone, so the entry
            // is metadata to tidy, and prunes the repository's share of it.
            prune_worktrees(repo);
            report.removed.push_back(entry.name);
            continue;
        }
        // Only a provably dead owner makes a candidate. Unknown -- no procfs,
        // no usable stamp -- must never reap, or a live window loses the
        // workspace it is working in.
        if (owner_state(entry) != OwnerState::Dead) {
            survivors.push_back(entry);
            continue;
        }
        std::string err;
        if (remove_worktree_safe(repo, entry.path, err)) {
            report.removed.push_back(entry.name);
        } else {
            report.kept.emplace_back(entry.name, err);
            survivors.push_back(entry);
        }
    }
    save_worktrees(state_dir, survivors);
    return report;
}

} // namespace agent
