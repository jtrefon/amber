
#include "agent/workspace.h"

#include "agent/config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace agent {

namespace fs = std::filesystem;

namespace {

// The XDG state root comes from the environment, so it gets the same sanity
// check as the config root: a relative or ".."-laden value must not redirect
// where amber writes.
bool is_sane_state_root(const std::string& path) {
    const fs::path p(path);
    if (!p.is_absolute())
        return false;
    return std::all_of(p.begin(), p.end(), [](const fs::path& part) { return part != ".."; });
}

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
    // Deliberately NOT created here: this is the project's shareable config
    // dir (skills, MCP servers, plugins), and merely running amber must not
    // create it -- in $HOME that used to leave a stray ~/.amber behind. Every
    // writer creates the directories it needs.
    return ensure_root() + "/.amber";
}

std::string workspace_state_slug(const std::string& workspace_path) {
    std::string slug;
    slug.reserve(workspace_path.size());
    for (char c : workspace_path) {
        const auto u = static_cast<unsigned char>(c);
        slug.push_back(std::isalnum(u) ? c : '-');
    }
    // Bound the name so a deep path cannot exceed a filesystem's filename
    // limit once the state dirs are appended.
    constexpr std::size_t kMaxSlug = 60;
    if (slug.size() > kMaxSlug)
        slug.resize(kMaxSlug);
    const auto trim_dashes = [](std::string& s) {
        const auto first = s.find_first_not_of('-');
        if (first == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(first, s.find_last_not_of('-') - first + 1);
    };
    trim_dashes(slug);
    if (slug.empty())
        slug = "project";
    // FNV-1a of the full path, always appended: sanitizing maps distinct paths
    // onto one name ("/a-b/c" and "/a/b-c" both become "a-b-c"), and two
    // projects sharing a state directory would silently share sessions.
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : workspace_path) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%08llx", h & 0xffffffffULL);
    return slug + suffix;
}

std::string Workspace::state_dir() {
    std::string base;
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && *xdg && is_sane_state_root(xdg)) {
        base = xdg;
    } else {
        const std::string home = user_home_dir();
        base = home.empty() ? ".amber-state" : home + "/.local/state";
    }
    return base + "/amber/projects/" + workspace_state_slug(ensure_root());
}

void Workspace::adopt_legacy_state(const std::string& name) {
    const std::string src = local_dir() + "/" + name;
    const std::string dst = state_dir() + "/" + name;
    std::error_code ec;
    if (!fs::exists(src, ec) || fs::exists(dst, ec))
        return;
    // Copy to a staging name and rename: a half-finished copy must never be
    // mistaken for the real thing on the next run. The parent is created
    // first -- the state tree does not exist on the first run after the move.
    const std::string tmp = dst + ".incoming";
    fs::create_directories(fs::path(dst).parent_path(), ec);
    fs::remove_all(tmp, ec);
    fs::copy(src, tmp, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        return;
    }
    fs::rename(tmp, dst, ec);
    if (ec)
        fs::remove_all(tmp, ec);
}

std::string Workspace::settings_path() {
    adopt_legacy_state("settings");
    return state_dir() + "/settings";
}

std::string Workspace::policy_path() {
    adopt_legacy_state("policy.json");
    return state_dir() + "/policy.json";
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
