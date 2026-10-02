
#include "agent/workspace.h"

#include <cstdlib>
#include <filesystem>

namespace agent {

namespace fs = std::filesystem;

namespace {

std::string& root_storage() {
    static std::string root;
    return root;
}

std::string normalize(const fs::path& p) {
    // lexically_normal collapses "." and ".." without touching the filesystem,
    // so confinement decisions do not depend on whether the path exists yet.
    return p.lexically_normal().generic_string();
}

std::string ensure_root() {
    std::string& r = root_storage();
    if (!r.empty())
        return r;
    if (const char* env = std::getenv("AMBER_WORKSPACE"); env && *env) {
        r = normalize(fs::absolute(fs::path(env)));
    } else {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        r = ec ? std::string(".") : normalize(cwd);
    }
    return r;
}

// True if `child` is `base` or a descendant of it, comparing normalized,
// slash-terminated prefixes so "/work/foo2" is not considered inside "/work/foo".
bool is_within(const std::string& base, const std::string& child) {
    if (child == base)
        return true;
    std::string b = base;
    if (b.empty() || b.back() != '/')
        b += '/';
    return child.compare(0, b.size(), b) == 0;
}

// The containment base is the canonicalized root: the root may itself be
// reached through a symlink (e.g. /tmp -> /private/tmp on macOS), and a
// lexical comparison against it would reject every in-workspace path.
std::string canonical_base(const std::string& base) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(base), ec);
    if (ec)
        canon = fs::path(base); // root does not exist yet
    return canon.generic_string();
}

// A dangling symlink leaf is a write escape — ofstream would create the target
// wherever the link points — so it is refused rather than resolved.
bool is_dangling_symlink(const fs::path& abs) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(abs, ec));
}

// The path does not exist yet: climb to the deepest existing ancestor and
// verify that ancestor's real location stays inside the root, never climbing
// above the root itself.
bool ancestor_stays_inside(const fs::path& abs, const fs::path& base, const std::string& cbase) {
    std::error_code ec;
    fs::path anchor = abs;
    while (true) {
        const fs::path canon = fs::weakly_canonical(anchor, ec);
        if (!ec)
            return is_within(cbase, canon.generic_string());
        if (anchor == base || anchor.parent_path() == base || anchor == anchor.root_path())
            return true; // the root itself does not exist: the lexical result stands
        anchor = anchor.parent_path();
    }
}

} // namespace

std::string Workspace::root() {
    return ensure_root();
}

void Workspace::set_root(const std::string& path) {
    root_storage() = normalize(fs::absolute(fs::path(path)));
}

std::string Workspace::local_dir() {
    std::string dir = ensure_root() + "/.amber";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

bool Workspace::confine(const std::string& path, std::string& resolved, std::string& error) {
    if (path.empty()) {
        error = "empty path";
        return false;
    }
    const std::string base = ensure_root();
    const fs::path req(path);
    const fs::path abs = req.is_absolute() ? req : (fs::path(base) / req);
    const std::string norm = normalize(abs);
    if (!is_within(base, norm)) {
        error = "path escapes workspace root (" + base + "): " + path;
        return false;
    }
    // A lexically-inside path is not enough: a symlink component can point
    // outside the root, and the tools open the path afterwards. The returned
    // path stays the lexical form so callers keep seeing the root prefix they
    // configured.
    const std::string cbase = canonical_base(base);

    // If the whole path exists, canonical() follows every component including
    // a symlink leaf; the real target must be inside the root.
    std::error_code ec;
    const fs::path canon_full = fs::canonical(abs, ec);
    if (!ec) {
        if (!is_within(cbase, canon_full.generic_string())) {
            error = "path resolves outside workspace root (" + base + "): " + path;
            return false;
        }
        resolved = norm;
        return true;
    }

    // canonical() failed: the path does not exist, or ends in a dangling
    // symlink, which is a write escape and is refused outright.
    if (is_dangling_symlink(abs)) {
        error = "path is a symlink whose target does not exist; refusing: " + path;
        return false;
    }

    if (!ancestor_stays_inside(abs, fs::path(base), cbase)) {
        error = "path resolves outside workspace root (" + base + "): " + path;
        return false;
    }
    resolved = norm;
    return true;
}

std::string Workspace::relative(const std::string& path) {
    std::string base = ensure_root();
    if (path.compare(0, base.size(), base) == 0 &&
        (path.size() == base.size() || path[base.size()] == '/')) {
        std::string rel = path.substr(base.size());
        if (!rel.empty() && rel[0] == '/')
            rel.erase(0, 1);
        return rel.empty() ? "." : rel;
    }
    return path;
}

} // namespace agent
