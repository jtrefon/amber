
#include "agent/skill_install.h"

#include "agent/archive_util.h"
#include "agent/skill_file.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace agent {

namespace {

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

} // namespace agent
