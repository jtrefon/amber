#include "agent/private_file.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/config.h"
#include "agent/config_paths.h"
#include "agent/workspace.h"

namespace agent {

namespace {

namespace fs = std::filesystem;

std::atomic<unsigned long> g_tmp_counter{0};

std::string parent_dir(const std::string& path) {
    return fs::path(path).parent_path().string();
}

// The path a write is actually allowed to touch: a canonical parent (realpath
// collapses "..", symlinks and duplicate separators) plus a single leaf that
// cannot itself traverse. This is the path-injection barrier CodeQL looks for,
// and it is also what keeps a provider name or session id from escaping its
// directory.
bool canonical_target(const std::string& path, std::string& out) {
    const fs::path p(path);
    const std::string leaf = p.filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..")
        return false;
    if (leaf.find('/') != std::string::npos || leaf.find('\\') != std::string::npos)
        return false;
    std::string parent = p.parent_path().string();
    if (parent.empty())
        parent = ".";
    char* resolved = ::realpath(parent.c_str(), nullptr);
    if (!resolved)
        return false;
    out = (fs::path(resolved) / leaf).string();
    std::free(resolved);
    return true;
}

bool narrow_mode(const std::string& path, mode_t mode) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec)
        return true; // absent is not a failure: nothing to secure
    if (status.type() == fs::file_type::symlink)
        return false; // never follow a link, and never chmod its target
    std::string target;
    if (!canonical_target(path, target))
        return false;
    return ::chmod(target.c_str(), mode) == 0;
}

bool ensure_parent_dir(const std::string& dir) {
    if (dir.empty())
        return true;
    std::error_code ec;
    const bool existed = fs::exists(dir, ec);
    fs::create_directories(dir, ec);
    if (ec)
        return false;
    // Only a directory this call created is narrowed implicitly. An existing
    // parent may be a shared location (/tmp, a project tree); the known amber
    // roots are narrowed explicitly by ensure_private_dir and migration.
    return existed || narrow_mode(dir, 0700);
}

int open_private(const std::string& path, int flags) {
    const std::string parent = parent_dir(path);
    if (!parent.empty() && !ensure_parent_dir(parent))
        return -1;
    std::string target;
    if (!canonical_target(path, target))
        return -1;
    const int fd = ::open(target.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    // O_CREAT only applies the mode at creation; narrow an existing file too.
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string temp_path(const std::string& path) {
    return path + ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
           std::to_string(g_tmp_counter.fetch_add(1));
}

void secure_files_in(const std::string& dir, const std::string& suffix) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!suffix.empty() && it->path().extension() != suffix)
            continue;
        const fs::file_status status = it->symlink_status(ec);
        if (ec)
            break;
        if (status.type() == fs::file_type::regular)
            secure_existing_file(it->path().string());
    }
}

} // namespace

bool ensure_private_dir(const std::string& path) {
    if (path.empty())
        return true;
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec)
        return false;
    return narrow_mode(path, 0700);
}

int open_private_append(const std::string& path) {
    return open_private(path, O_WRONLY | O_CREAT | O_APPEND);
}

int open_private_truncate(const std::string& path) {
    return open_private(path, O_WRONLY | O_CREAT | O_TRUNC);
}

bool write_all(int fd, const std::string& bytes) {
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_private_file(const std::string& path, const std::string& bytes) {
    const std::string parent = parent_dir(path);
    if (!parent.empty() && !ensure_parent_dir(parent))
        return false;
    std::string target;
    if (!canonical_target(path, target))
        return false;
    const std::string tmp = temp_path(target);
    const int fd = open_private_truncate(tmp);
    if (fd < 0)
        return false;
    const bool wrote = write_all(fd, bytes);
    const bool synced = wrote && ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool secure_existing_file(const std::string& path) {
    return narrow_mode(path, 0600);
}

bool secure_existing_dir(const std::string& path) {
    return narrow_mode(path, 0700);
}

void secure_private_state() {
    try {
        const std::string config_dir = global_config_dir();
        secure_existing_dir(config_dir);
        secure_existing_file(global_config_path());
        const std::string providers = config_dir + "/providers";
        secure_existing_dir(providers);
        secure_files_in(providers, ".conf");

        const std::string state = Workspace::state_dir();
        secure_existing_dir(state);
        const std::string sessions = state + "/sessions";
        secure_existing_dir(sessions);
        secure_files_in(sessions, ".json");

        // The transcript directory is narrowed, not walked: it can hold tens of
        // thousands of files, and 0700 removes every other user's traversal
        // without a startup scan proportional to history.
        secure_existing_dir(state + "/logs");
        secure_existing_file(error_log_path());
        const std::string errors = state + "/errors";
        secure_existing_dir(errors);
        secure_files_in(errors, "");

        secure_existing_dir(state + "/artifacts");
        const std::string settings = Workspace::settings_path();
        secure_existing_dir(parent_dir(settings));
        secure_existing_file(settings);
        const std::string policy = Workspace::policy_path();
        secure_existing_dir(parent_dir(policy));
        secure_existing_file(policy);
        const std::string experience = state + "/experience.json";
        secure_existing_file(experience);
    } catch (...) {
        // Startup must not fail because a mode could not be narrowed; the next
        // write still uses the private primitive.
    }
}

void secure_private_state_async() {
    static std::future<void> pass = std::async(std::launch::async, [] { secure_private_state(); });
    (void)pass;
}

} // namespace agent
