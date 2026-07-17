// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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
    if (const char* env = std::getenv("CPP_AGENT_WORKSPACE"); env && *env) {
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
    resolved = norm;
    return true;
}

} // namespace agent
