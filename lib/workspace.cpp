
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
    if (!r.empty()) return r;
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
    if (child == base) return true;
    std::string b = base;
    if (b.empty() || b.back() != '/') b += '/';
    return child.compare(0, b.size(), b) == 0;
}

} // namespace

std::string Workspace::root() { return ensure_root(); }

void Workspace::set_root(const std::string& path) {
    root_storage() = normalize(fs::absolute(fs::path(path)));
}

std::string Workspace::local_dir() {
    std::string dir = ensure_root() + "/.amber";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

bool Workspace::confine(const std::string& path, std::string& resolved,
                        std::string& error) {
    if (path.empty()) {
        error = "empty path";
        return false;
    }
    const std::string base = ensure_root();
    fs::path req(path);
    fs::path abs = req.is_absolute() ? req : (fs::path(base) / req);
    std::string norm = normalize(abs);
    if (!is_within(base, norm)) {
        error = "path escapes workspace root (" + base + "): " + path;
        return false;
    }
    // A lexically-inside path is not enough: a symlink component can point
    // outside the root, and the tools open the path afterwards. Resolve the
    // real target of the deepest existing prefix and verify it stays inside
    // the root; the returned path remains the lexical form so callers keep
    // seeing the root prefix they configured.
    //
    // The containment base is the canonicalized root: the root may itself be
    // reached through a symlink (e.g. /tmp -> /private/tmp on macOS), and a
    // lexical comparison against it would reject every in-workspace path.
    std::error_code ec;
    fs::path canon_base = fs::weakly_canonical(fs::path(base), ec);
    if (ec) canon_base = fs::path(base);   // root does not exist yet
    const std::string cbase = canon_base.generic_string();

    // If the whole path exists, canonical() follows every component including
    // a symlink leaf; the real target must be inside the root.
    fs::path canon_full = fs::canonical(abs, ec);
    if (!ec) {
        if (!is_within(cbase, canon_full.generic_string())) {
            error = "path resolves outside workspace root (" + base +
                    "): " + path;
            return false;
        }
        resolved = norm;
        return true;
    }

    // canonical() failed: the path does not exist, or ends in a dangling
    // symlink. A dangling symlink leaf is a write escape — ofstream would
    // create the target wherever the link points — so refuse it outright.
    {
        std::error_code lec;
        if (fs::is_symlink(fs::symlink_status(abs, lec))) {
            error = "path is a symlink whose target does not exist; "
                    "refusing: " + path;
            return false;
        }
    }

    // The path does not exist yet. Climb to the deepest existing ancestor and
    // verify that ancestor's real location stays inside the root.
    fs::path anchor = abs;
    while (true) {
        fs::path canon = fs::weakly_canonical(anchor, ec);
        if (!ec) {
            if (!is_within(cbase, canon.generic_string())) {
                error = "path resolves outside workspace root (" + base +
                        "): " + path;
                return false;
            }
            resolved = norm;
            return true;
        }
        // Anchor does not exist. Peel its last component and resolve the next
        // existing ancestor, but never climb above the root itself.
        if (anchor == fs::path(base) ||
            anchor.parent_path() == fs::path(base) ||
            anchor == anchor.root_path()) {
            // The root itself does not exist (or does not resolve), so no
            // symlink can be hiding in it yet — the lexical result stands.
            resolved = norm;
            return true;
        }
        anchor = anchor.parent_path();
    }
}

std::string Workspace::relative(const std::string& path) {
    std::string base = ensure_root();
    if (path.compare(0, base.size(), base) == 0 &&
        (path.size() == base.size() || path[base.size()] == '/')) {
        std::string rel = path.substr(base.size());
        if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        return rel.empty() ? "." : rel;
    }
    return path;
}

} // namespace agent
