
#include "agent/skill_install.h"

#include "agent/archive_util.h"
#include "agent/skill_file.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <unistd.h>

namespace agent {

namespace {

constexpr size_t kMaxSkillFiles = 200;
constexpr long long kMaxSkillBytes = 10LL * 1024 * 1024;

std::string stamp() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(getpid()) + "-" + std::to_string(ticks);
}

void remove_tree(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

bool safe_relative(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string segment =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool check_limits(const std::vector<SkillPayloadFile>& files, std::string& err) {
    if (files.empty() || files.size() > kMaxSkillFiles) {
        err = "skill has an invalid number of files";
        return false;
    }
    long long bytes = 0;
    for (const auto& f : files) {
        bytes += static_cast<long long>(f.contents.size());
        if (bytes > kMaxSkillBytes) {
            err = "skill exceeds the size cap";
            return false;
        }
    }
    return true;
}

bool validate(const SkillInstallRequest& req, std::string& err) {
    if (!is_kebab_name(req.name)) {
        err = "invalid skill name: " + req.name;
        return false;
    }
    if (req.dest_root.empty()) {
        err = "no skills root";
        return false;
    }
    if (!check_limits(req.files, err))
        return false;

    std::set<std::string> seen;
    bool has_skill_md = false;
    for (const auto& f : req.files) {
        if (!safe_relative(f.path)) {
            err = "unsafe path: " + f.path;
            return false;
        }
        if (!seen.insert(f.path).second) {
            err = "duplicate path: " + f.path;
            return false;
        }
        if (f.path == "SKILL.md") {
            has_skill_md = true;
            if (!parse_skill_meta(f.contents)) {
                err = "malformed SKILL.md";
                return false;
            }
        }
    }
    if (!has_skill_md) {
        err = "SKILL.md missing at the skill root";
        return false;
    }
    return true;
}

bool write_asset(const std::string& root, const SkillPayloadFile& f, std::string& err) {
    const std::filesystem::path path = std::filesystem::path(root) / f.path;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        err = "cannot create " + path.parent_path().string();
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot write " + path.string();
        return false;
    }
    out.write(f.contents.data(), static_cast<std::streamsize>(f.contents.size()));
    if (!out) {
        err = "short write " + path.string();
        return false;
    }
    return true;
}

bool move_into_place(const std::string& temp, const std::string& dest, bool replace,
                     std::string& err) {
    std::error_code ec;
    if (!std::filesystem::exists(dest)) {
        std::filesystem::rename(temp, dest, ec);
        if (ec)
            err = "cannot move skill into place: " + ec.message();
        return !ec;
    }
    if (!replace) {
        err = "skill already installed: " + dest;
        return false;
    }

    const std::string old = dest + ".old-" + stamp();
    std::filesystem::rename(dest, old, ec);
    if (ec) {
        err = "cannot replace " + dest + ": " + ec.message();
        return false;
    }
    std::filesystem::rename(temp, dest, ec);
    if (!ec) {
        remove_tree(old);
        return true;
    }
    std::error_code restore;
    std::filesystem::rename(old, dest, restore);
    err = "cannot replace " + dest + ": " + ec.message();
    return false;
}

// Find the skill name: the single top-level directory containing SKILL.md.
// Returns "" when the archive layout is not a skill pack.
std::string pack_skill_name(const std::string& listing) {
    std::istringstream ss(listing);
    std::string entry;
    while (std::getline(ss, entry)) {
        if (entry.size() < 9)
            continue;
        if (entry.compare(entry.size() - 9, 9, "/SKILL.md") != 0)
            continue;
        size_t first = entry.find_first_not_of("./");
        size_t slash = entry.find('/', first);
        if (first == std::string::npos || slash == std::string::npos)
            return "";
        return entry.substr(first, slash - first);
    }
    return "";
}

} // namespace

// Stage the archive into a private temp dir and unpack it. On success `tmp` and
// `name` describe the staged pack and the error is empty; on failure the staging
// is already cleaned up.
std::string stage_skill_pack(const std::string& bytes, std::string& tmp, std::string& name) {
    tmp = "/tmp/amber-skill-install-" + std::to_string(getpid());
    const std::string tgz = tmp + ".tar.gz";
    const auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tgz, ec);
    };
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    {
        std::ofstream f(tgz, std::ios::binary);
        f << bytes;
    }
    name = pack_skill_name(list_tar_gz(tgz));
    if (name.empty()) {
        cleanup();
        return "archive is not a skill pack (SKILL.md must sit inside one "
               "top-level directory)";
    }
    if (!is_kebab_name(name)) {
        const std::string bad = name;
        cleanup();
        return "invalid skill name '" + bad + "'";
    }
    if (!unpack_tar_gz(tgz, tmp).empty()) {
        cleanup();
        return "cannot unpack archive";
    }
    std::ifstream body_in(tmp + "/" + name + "/SKILL.md", std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(body_in)),
                           std::istreambuf_iterator<char>());
    if (!parse_skill_meta(body)) {
        cleanup();
        return "malformed SKILL.md (missing or invalid frontmatter)";
    }
    return "";
}

std::string install_skill_pack(const std::string& source, const std::string& dest_root) {
    std::string err;
    const std::string bytes = fetch_bytes(source, err);
    if (bytes.empty())
        return err;

    std::string tmp;
    std::string name;
    std::string stage_error = stage_skill_pack(bytes, tmp, name);
    if (!stage_error.empty())
        return stage_error;

    std::error_code ec;
    std::filesystem::create_directories(dest_root, ec);
    const std::string dest = dest_root + "/" + name;
    std::filesystem::remove_all(dest, ec);
    std::error_code ec2;
    std::filesystem::copy(tmp + "/" + name, dest, std::filesystem::copy_options::recursive, ec2);
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(tmp + ".tar.gz", ec);
    if (ec2)
        return "cannot stage skill: " + ec2.message();
    return "";
}

std::string uninstall_skill(const std::string& name, const std::string& dest_root) {
    if (!is_kebab_name(name))
        return "invalid skill name: " + name;
    std::string dest = dest_root + "/" + name;
    if (!std::filesystem::exists(dest))
        return "skill not installed: " + name;
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    if (ec)
        return "cannot remove " + dest;
    return "";
}

bool is_safe_skill_path(const std::string& relative_path) noexcept {
    return safe_relative(relative_path);
}

std::string skill_content_hash(const std::string& contents) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : contents) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

namespace {

bool absorb_skill_file(const std::filesystem::path& dir, const std::filesystem::path& path,
                       LedgerEntry& entry, bool& has_skill_md) {
    if (std::filesystem::is_symlink(path))
        return false;
    if (!std::filesystem::is_regular_file(path))
        return true;

    std::error_code ec;
    const std::string relative = std::filesystem::relative(path, dir, ec).generic_string();
    if (ec || !is_safe_skill_path(relative))
        return false;

    ++entry.files;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec)
        entry.bytes += static_cast<long long>(size);

    if (relative != "SKILL.md")
        return true;
    std::ifstream in(path, std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parse_skill_meta(body))
        return false;
    entry.local_hash = skill_content_hash(body);
    has_skill_md = true;
    return true;
}

} // namespace

LedgerEntry ledger_entry_for(const SkillDetail& detail, const std::string& name,
                             const std::vector<SkillPayloadFile>& files,
                             const SkillInstallResult& result, bool enabled) {
    LedgerEntry entry;
    entry.id = detail.ref.id;
    entry.source = detail.ref.source;
    entry.slug = name;
    entry.ref = detail.default_branch;
    entry.hash = detail.hash;
    entry.category = detail.category;
    for (const auto& file : files) {
        if (file.path == "SKILL.md")
            entry.local_hash = skill_content_hash(file.contents);
    }
    entry.installed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    entry.files = result.files;
    entry.bytes = result.bytes;
    entry.enabled = enabled;
    return entry;
}

std::optional<LedgerEntry> adopt_skill(const std::string& root, const std::string& name) {
    if (!is_kebab_name(name))
        return std::nullopt;
    const std::filesystem::path dir = std::filesystem::path(root) / name;
    if (!std::filesystem::is_directory(dir))
        return std::nullopt;

    LedgerEntry entry;
    entry.slug = name;
    entry.installed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    entry.enabled = true;

    bool has_skill_md = false;
    std::error_code ec;
    for (const auto& it : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec || !absorb_skill_file(dir, it.path(), entry, has_skill_md))
            return std::nullopt;
    }
    if (!has_skill_md)
        return std::nullopt;
    return entry;
}

SkillInstallResult install_skill_files(const SkillInstallRequest& request) {
    SkillInstallResult result;
    if (!validate(request, result.error))
        return result;

    std::error_code ec;
    std::filesystem::create_directories(request.dest_root, ec);
    if (ec) {
        result.error = "cannot create skills root: " + ec.message();
        return result;
    }

    const std::string dest = request.dest_root + "/" + request.name;
    const std::string temp = request.dest_root + "/.install-" + request.name + "-" + stamp();
    remove_tree(temp);

    for (const auto& file : request.files) {
        if (!write_asset(temp, file, result.error)) {
            remove_tree(temp);
            return result;
        }
        ++result.files;
        result.bytes += static_cast<long long>(file.contents.size());
    }

    if (!move_into_place(temp, dest, request.replace, result.error)) {
        remove_tree(temp);
        result.files = 0;
        result.bytes = 0;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace agent
